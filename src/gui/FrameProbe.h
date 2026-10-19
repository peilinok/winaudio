#pragma once
#include <cstdint>

namespace wa {

// One GUI-frame slice. Several hits in the same frame share one slot.
enum class FrameSlice : int {
    Heatmap = 0,
    Envelope = 1,
    LogDrain = 2,
    Present = 3,
    Count = 4,
};

// Fixed slots for one main-window frame. Off by default.
// Disabled begin/end do not call QueryPerformanceCounter.
// Enabled begin/end only add the bracket into the slot: no heap, no logging.
// uncertainty_ticks on a formatted line is the hit count, because each
// bracket is ±1 tick and the line reports their sum.
// Machine facts are never filled in.
class FrameProbe {
public:
    struct Slot {
        uint64_t ticks = 0;
        uint32_t n = 0;
    };

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    // Drops the previous frame. No-op while disabled.
    void beginFrame();
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
