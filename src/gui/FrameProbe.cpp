#include "FrameProbe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace wa {
namespace {

const char* buildName() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

uint64_t nanosFromTicks(uint64_t ticks, uint64_t freq) {
    if (freq == 0) return 0;
    const uint64_t whole = ticks / freq;
    const uint64_t rem = ticks % freq;
    return whole * 1000000000ull + (rem * 1000000000ull) / freq;
}

int sliceIndex(FrameSlice slice) {
    const int i = static_cast<int>(slice);
    if (i < 0 || i >= static_cast<int>(FrameSlice::Count)) return -1;
    return i;
}

uint64_t qpcNow() {
    LARGE_INTEGER now{};
    return QueryPerformanceCounter(&now) ? static_cast<uint64_t>(now.QuadPart) : 0;
}

int copyLine(const char* line, int size, char* buf, int capacity) {
    if (size <= 0 || !buf || size >= capacity) return 0;
    std::memcpy(buf, line, static_cast<size_t>(size) + 1);
    return size;
}

}  // namespace

FrameProbe::FrameProbe(Clock clock, uint64_t frequency)
    : clock_(clock ? clock : qpcNow), qpcFreq_(frequency) {}

void FrameProbe::setEnabled(bool on) {
    if (on && qpcFreq_ == 0) {
        LARGE_INTEGER freq;
        if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0)
            qpcFreq_ = static_cast<uint64_t>(freq.QuadPart);
    }
    enabled_ = on;
}

void FrameProbe::beginFrame() {
    if (!enabled_) return;
    if (frameOpen_) ++lostFrames_;
    for (int i = 0; i < kCount; ++i) {
        slots_[i].ticks = 0;
        slots_[i].n = 0;
        slots_[i].lost = open_[i] ? 1u : 0u;
        open_[i] = false;
        openStart_[i] = 0;
    }
    const uint64_t now = clock_();
    frameInterval_ = frameNumber_ ? now - frameStart_ : 0;
    frameStart_ = now;
    ++frameNumber_;
    operationCount_ = 0;
    workCount_ = 0;
    frameOpen_ = true;
    openStart_[static_cast<int>(FrameSlice::MainThread)] = now;
    open_[static_cast<int>(FrameSlice::MainThread)] = true;
}

void FrameProbe::endFrame() {
    if (!enabled_ || !frameOpen_) return;
    end(FrameSlice::MainThread);
    const uint64_t endTick = openStart_[static_cast<int>(FrameSlice::MainThread)] +
                             slot(FrameSlice::MainThread).ticks;
    for (size_t i = 0; i < pendingOperationCount_;) {
        if (!pendingOperations_[i].ready) { ++i; continue; }
        Operation op = pendingOperations_[i];
        op.end = endTick;
        op.frame = frameNumber_;
        operations_[operationCount_++] = op;
        for (size_t j = i + 1; j < pendingOperationCount_; ++j)
            pendingOperations_[j - 1] = pendingOperations_[j];
        --pendingOperationCount_;
    }
    frameOpen_ = false;
    // An unclaimed press must not be attached to an unrelated later action.
    inputTick_ = 0;
}

bool FrameProbe::startOperation(uint64_t id, const char* kind, uint64_t start) {
    if (!enabled_) return false;
    if (pendingOperationCount_ == pendingOperations_.size()) {
        ++lostOperations_;
        return false;
    }
    for (size_t i = 0; i < pendingOperationCount_; ++i)
        if (pendingOperations_[i].id == id) return false;
    pendingOperations_[pendingOperationCount_++] = {id, start, 0, 0, kind, false};
    return true;
}

void FrameProbe::noteInput(uint32_t message, uint32_t ageMilliseconds) {
    if (!enabled_ || (message != WM_LBUTTONDOWN && message != WM_LBUTTONDBLCLK)) return;
    // ImGui may trickle buffered presses over multiple frames. A single input
    // marker cannot associate them reliably: reject this run rather than reuse
    // the latest timestamp for the first click.
    if (inputTick_) { ++lostOperations_; return; }
    const uint64_t now = clock_();
    const uint64_t age = static_cast<uint64_t>(ageMilliseconds) * qpcFreq_ / 1000;
    inputTick_ = age <= now ? now - age : 0;
}

void FrameProbe::pageState(const char* target, bool clicked, bool visible) {
    if (visible) scene_ = target;
    if (!enabled_) return;
    if (clicked && !inputTick_) ++lostOperations_;
    if (clicked && inputTick_) {
        const uint64_t id = ++nextOperationId_;
        if (startOperation(id, target, inputTick_)) {
            pendingPageId_ = id;
            pendingPageTarget_ = target;
        }
        inputTick_ = 0;
    }
    if (visible && pendingPageId_ && std::strcmp(pendingPageTarget_, target) == 0) {
        operationReady(pendingPageId_);
        pendingPageId_ = 0;
        pendingPageTarget_ = nullptr;
    }
}

void FrameProbe::interactionReady(const char* kind) {
    if (!enabled_ || !inputTick_) return;
    const uint64_t id = ++nextOperationId_;
    if (startOperation(id, kind, inputTick_)) operationReady(id);
    inputTick_ = 0;
}

void FrameProbe::addWork(const char* name, uint64_t value, uint64_t source,
                         int direction, int channel) {
    if (!enabled_) return;
    if (workCount_ == work_.size()) { ++lostWork_; return; }
    work_[workCount_++] = {name, value, source, direction, channel};
}

int FrameProbe::formatFrame(char* buf, int bufSize) const {
    if (!enabled_ || !frameNumber_) return 0;
    uint64_t mask = 0;
    uint32_t lost = 0;
    for (int i = 0; i < kCount; ++i) {
        if (slots_[i].n) mask |= uint64_t{1} << i;
        lost += slots_[i].lost + (open_[i] ? 1u : 0u);
    }
    char line[1024];
    const int n = std::snprintf(line, sizeof(line),
        "kind=frame schema=2 frame=%llu start=%llu interval_ticks=%llu qpc_freq=%llu "
        "scene=%s stage=%s valid_mask=%llu lost_slices=%u lost_operations=%u "
        "lost_work=%u lost_frames=%u pending_operations=%zu\n",
        static_cast<unsigned long long>(frameNumber_),
        static_cast<unsigned long long>(frameStart_),
        static_cast<unsigned long long>(frameInterval_),
        static_cast<unsigned long long>(qpcFreq_), scene_, stage_,
        static_cast<unsigned long long>(mask), lost, lostOperations_, lostWork_,
        lostFrames_, pendingOperationCount_);
    return n < static_cast<int>(sizeof(line)) ? copyLine(line, n, buf, bufSize) : 0;
}

int FrameProbe::formatOperation(size_t index, char* buf, int bufSize) const {
    if (index >= operationCount_) return 0;
    const auto& op = operations_[index];
    char line[512];
    const int n = std::snprintf(line, sizeof(line),
        "kind=operation frame=%llu id=%llu target=%s start=%llu end=%llu "
        "ticks=%llu qpc_freq=%llu scene=%s stage=%s\n",
        static_cast<unsigned long long>(op.frame), static_cast<unsigned long long>(op.id),
        op.kind, static_cast<unsigned long long>(op.start),
        static_cast<unsigned long long>(op.end),
        static_cast<unsigned long long>(op.end - op.start),
        static_cast<unsigned long long>(qpcFreq_), scene_, stage_);
    return n < static_cast<int>(sizeof(line)) ? copyLine(line, n, buf, bufSize) : 0;
}

int FrameProbe::formatWork(size_t index, char* buf, int bufSize) const {
    if (index >= workCount_) return 0;
    const auto& work = work_[index];
    char line[512];
    const int n = std::snprintf(line, sizeof(line),
        "kind=work frame=%llu metric=%s value=%llu source=%llu direction=%d channel=%d\n",
        static_cast<unsigned long long>(frameNumber_), work.name,
        static_cast<unsigned long long>(work.value),
        static_cast<unsigned long long>(work.source), work.direction, work.channel);
    return n < static_cast<int>(sizeof(line)) ? copyLine(line, n, buf, bufSize) : 0;
}

void FrameProbe::operationReady(uint64_t id) {
    if (!enabled_) return;
    for (size_t i = 0; i < pendingOperationCount_; ++i)
        if (pendingOperations_[i].id == id) pendingOperations_[i].ready = true;
}

void FrameProbe::begin(FrameSlice slice) {
    if (!enabled_) return;
    const int i = sliceIndex(slice);
    if (i < 0) return;
    if (open_[i]) ++slots_[i].lost;
    openStart_[i] = clock_();
    open_[i] = true;
}

void FrameProbe::end(FrameSlice slice) {
    if (!enabled_) return;
    const int i = sliceIndex(slice);
    if (i < 0 || !open_[i]) return;
    const uint64_t endTick = clock_();
    const uint64_t startTick = openStart_[i];
    open_[i] = false;
    if (endTick < startTick) { ++slots_[i].lost; return; }
    if (endTick > startTick)
        slots_[i].ticks += endTick - startTick;
    slots_[i].n += 1;
}

const FrameProbe::Slot& FrameProbe::slot(FrameSlice slice) const {
    static const Slot kEmpty{};
    const int i = sliceIndex(slice);
    if (i < 0) return kEmpty;
    return slots_[i];
}

int FrameProbe::formatLine(FrameSlice slice, char* buf, int bufSize) const {
    const int i = sliceIndex(slice);
    if (i < 0 || slots_[i].n == 0) return 0;
    if (!buf || bufSize <= 1) return 0;
    const Slot& s = slots_[i];
    const uint64_t nanos = nanosFromTicks(s.ticks, qpcFreq_);
    // Format away from the caller buffer so a short buffer stays unchanged.
    char line[1024];
    const int wrote = std::snprintf(
        line, sizeof(line),
        "slice=%s build=%s N=%u ticks=%llu qpc_freq=%llu nanos=%llu "
        "uncertainty_ticks=%u scene=%s schema=2 frame=%llu stage=%s lost=%u "
        "command=set WINAUDIO_FRAME_PROBE=1 & .\\build\\bin\\%s\\WinAudioGui.exe\n",
        sliceName(slice), buildName(), s.n,
        static_cast<unsigned long long>(s.ticks),
        static_cast<unsigned long long>(qpcFreq_),
        static_cast<unsigned long long>(nanos),
        s.n, scene_, static_cast<unsigned long long>(frameNumber_), stage_, s.lost, buildName());
    if (wrote <= 0 || wrote >= static_cast<int>(sizeof(line)) || wrote >= bufSize)
        return 0;
    std::memcpy(buf, line, static_cast<size_t>(wrote) + 1);
    return wrote;
}

const char* FrameProbe::headerLine() {
    return
        "# WinAudio frame probe. Each line is one slice. "
        "schema=2; scene is supplied by AppUi; the probe does not detect it itself. "
        "Machine facts are required and are not detected: "
        "CPU, GPU, memory, refresh_hz, power_mode, windows. "
        "Without them this file is not low-end evidence.\n";
}

const char* FrameProbe::sliceName(FrameSlice slice) {
    switch (slice) {
    case FrameSlice::Heatmap: return "heatmap-submit";
    case FrameSlice::Envelope: return "waveform-envelope";
    case FrameSlice::LogDrain: return "log-drain";
    case FrameSlice::Present: return "present";
    case FrameSlice::MainThread: return "main-thread";
    case FrameSlice::Ui: return "ui-build";
    case FrameSlice::DrawSubmit: return "dx11-submit";
    case FrameSlice::ProbeIo: return "probe-io";
    case FrameSlice::ChartRefresh: return "chart-refresh";
    case FrameSlice::Fft: return "fft";
    case FrameSlice::HeatmapFill: return "heatmap-fill";
    case FrameSlice::HeatmapUpload: return "heatmap-upload";
    case FrameSlice::CallReceive: return "call-receive";
    case FrameSlice::CallSort: return "call-sort";
    case FrameSlice::CallMatch: return "call-match";
    case FrameSlice::CallRows: return "call-rows";
    case FrameSlice::GraphHint: return "graph-hint";
    case FrameSlice::ExportStart: return "export-start";
    case FrameSlice::Count: break;
    }
    return "unknown";
}

const char* FrameProbe::sceneLabel() {
    return "unlabelled";
}

}  // namespace wa
