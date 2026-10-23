#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace wa {

// One GUI-frame slice. Several hits in the same frame share one slot.
enum class FrameSlice : int {
    Heatmap = 0,
    Envelope = 1,
    LogDrain = 2,
    Present = 3,
    MainThread = 4,
    Ui = 5,
    DrawSubmit = 6,
    ProbeIo = 7,
    ChartRefresh = 8,
    Fft = 9,
    HeatmapFill = 10,
    HeatmapUpload = 11,
    CallReceive = 12,
    CallSort = 13,
    CallMatch = 14,
    CallRows = 15,
    GraphHint = 16,
    ExportStart = 17,
    Count = 18,
};

// Fixed slots for one main-window frame. Off by default.
// Disabled begin/end do not call QueryPerformanceCounter.
// Enabled begin/end only add the bracket into the slot: no heap, no logging.
// uncertainty_ticks on a formatted line is the hit count, because each
// bracket is ±1 tick and the line reports their sum.
// Machine facts are never filled in.
class FrameProbe {
public:
    using Clock = uint64_t (*)();
    explicit FrameProbe(Clock clock = nullptr, uint64_t frequency = 0);
    struct Slot {
        uint64_t ticks = 0;
        uint32_t n = 0;
        uint32_t lost = 0;
    };
    struct Operation {
        uint64_t id = 0, start = 0, end = 0, frame = 0;
        const char* kind = "unknown";
        bool ready = false;
    };
    struct Work {
        const char* name = "unknown";
        uint64_t value = 0, source = 0;
        int direction = -1, channel = -1;
    };

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    // Drops the previous frame. No-op while disabled.
    void beginFrame();
    void endFrame();
    uint64_t frameNumber() const { return frameNumber_; }
    uint64_t frameIntervalTicks() const { return frameInterval_; }
    // start is the issue timestamp, not the time the UI dequeues the input.
    bool startOperation(uint64_t id, const char* kind, uint64_t start);
    void operationReady(uint64_t id);
    size_t operationCount() const { return operationCount_; }
    const Operation& operation(size_t index) const { return operations_[index]; }
    // Labels must outlive their frame (or pending operation) and contain no whitespace.
    void setScene(const char* scene) { scene_ = scene; }
    void setStage(const char* stage) { stage_ = stage; }
    void addWork(const char* name, uint64_t value, uint64_t source = 0,
                 int direction = -1, int channel = -1);
    size_t workCount() const { return workCount_; }
    int formatFrame(char* buf, int bufSize) const;
    int formatOperation(size_t index, char* buf, int bufSize) const;
    int formatWork(size_t index, char* buf, int bufSize) const;
    // Page actions are triggered by left-button press. Unrelated messages do
    // not replace that timestamp. Ambiguous buffered presses are reported as
    // lost and make the run unsuitable for acceptance. Other operation sources
    // use startOperation.
    void noteInput(uint32_t message, uint32_t ageMilliseconds);
    void pageState(const char* target, bool clicked, bool visible);
    void interactionReady(const char* kind);
    void begin(FrameSlice slice);
    void end(FrameSlice slice);

    const Slot& slot(FrameSlice slice) const;
    uint64_t qpcFrequency() const { return qpcFreq_; }

    // Bytes written, excluding the NUL, or 0 when n is 0 or buf cannot hold the line.
    // A 0 return leaves buf unchanged. The line does not contain machine facts.
    int formatLine(FrameSlice slice, char* buf, int bufSize) const;

    static const char* headerLine();
    static const char* sliceName(FrameSlice slice);
    static const char* sceneLabel();

private:
    static constexpr int kCount = static_cast<int>(FrameSlice::Count);
    bool enabled_ = false;
    Clock clock_;
    uint64_t frameNumber_ = 0;
    uint64_t frameStart_ = 0;
    uint64_t frameInterval_ = 0;
    std::array<Operation, 16> pendingOperations_{}, operations_{};
    size_t pendingOperationCount_ = 0, operationCount_ = 0;
    std::array<Work, 256> work_{};
    size_t workCount_ = 0;
    uint64_t inputTick_ = 0, nextOperationId_ = 0;
    uint64_t pendingPageId_ = 0;
    const char* pendingPageTarget_ = nullptr;
    uint32_t lostOperations_ = 0, lostWork_ = 0, lostFrames_ = 0;
    bool frameOpen_ = false;
    const char* scene_ = "unlabelled";
    const char* stage_ = "manual";
    uint64_t qpcFreq_ = 0;
    Slot slots_[kCount]{};
    uint64_t openStart_[kCount]{};
    bool open_[kCount]{};
};

// Closes the bracket on every exit. While the probe is disabled both calls return immediately.
class FrameBracket {
public:
    FrameBracket(FrameProbe& probe, FrameSlice slice) : probe_(probe), slice_(slice) {
        probe_.begin(slice_);
    }
    ~FrameBracket() { probe_.end(slice_); }
    FrameBracket(const FrameBracket&) = delete;
    FrameBracket& operator=(const FrameBracket&) = delete;

private:
    FrameProbe& probe_;
    FrameSlice slice_;
};

}  // namespace wa
