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

}  // namespace

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
    for (int i = 0; i < kCount; ++i) {
        slots_[i].ticks = 0;
        slots_[i].n = 0;
        open_[i] = false;
        openStart_[i] = 0;
    }
}

void FrameProbe::begin(FrameSlice slice) {
    if (!enabled_) return;
    const int i = sliceIndex(slice);
    if (i < 0) return;
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) return;
    openStart_[i] = static_cast<uint64_t>(now.QuadPart);
    open_[i] = true;
}

void FrameProbe::end(FrameSlice slice) {
    if (!enabled_) return;
    const int i = sliceIndex(slice);
    if (i < 0 || !open_[i]) return;
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) {
        open_[i] = false;
        return;
    }
    const uint64_t endTick = static_cast<uint64_t>(now.QuadPart);
    const uint64_t startTick = openStart_[i];
    open_[i] = false;
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
    char line[512];
    const int wrote = std::snprintf(
        line, sizeof(line),
        "slice=%s build=%s N=%u ticks=%llu qpc_freq=%llu nanos=%llu "
        "uncertainty_ticks=%u scene=%s "
        "command=set WINAUDIO_FRAME_PROBE=1 & .\\build\\bin\\%s\\WinAudioGui.exe\n",
        sliceName(slice), buildName(), s.n,
        static_cast<unsigned long long>(s.ticks),
        static_cast<unsigned long long>(qpcFreq_),
        static_cast<unsigned long long>(nanos),
        s.n, sceneLabel(), buildName());
    if (wrote <= 0 || wrote >= static_cast<int>(sizeof(line)) || wrote >= bufSize)
        return 0;
    std::memcpy(buf, line, static_cast<size_t>(wrote) + 1);
    return wrote;
}

const char* FrameProbe::headerLine() {
    return
        "# WinAudio frame probe. Each line is one slice. "
        "The scene field is the reference scene; this probe does not detect which page is showing. "
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
    case FrameSlice::Count: break;
    }
    return "unknown";
}

const char* FrameProbe::sceneLabel() {
    return "Render-page;chartChannels=8;spectrograms=scrolling;log=default-expanded";
}

}  // namespace wa
