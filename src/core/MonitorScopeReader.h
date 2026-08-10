#pragma once
#include "MonitorEngine.h"
#include "ScopeReader.h"

namespace wa {

// Production Scope Reader over MonitorEngine capture or render scope taps.
class MonitorScopeReader final : public ScopeReader {
public:
    enum class Side { Capture, Render };

    MonitorScopeReader(MonitorEngine& engine, Side side) : engine_(engine), side_(side) {}

    uint64_t written() const override {
        return side_ == Side::Capture ? engine_.capWritten() : engine_.renderWritten();
    }

    uint16_t channels() const override {
        if (side_ == Side::Capture) return engine_.captureScopeChannels();
        return 1; // render scope is mono
    }

    bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const override {
        return side_ == Side::Capture ? engine_.snapshotCapture(n, out, endIdxOut)
                                      : engine_.snapshotRender(n, out, endIdxOut);
    }

    bool snapshotEndingAt(uint64_t endIdx, size_t n, float* out) const override {
        return side_ == Side::Capture ? engine_.snapshotCaptureAt(endIdx, n, out)
                                      : engine_.snapshotRenderAt(endIdx, n, out);
    }

    bool snapshotChannelEndingAt(uint16_t channel, uint64_t endIdx, size_t n,
                                 float* out) const override {
        if (side_ == Side::Render) {
            // Render tap is mono: channel 0 only.
            if (channel != 0) return false;
            return engine_.snapshotRenderAt(endIdx, n, out);
        }
        return engine_.snapshotCaptureChannelAt(channel, endIdx, n, out);
    }

private:
    MonitorEngine& engine_;
    Side           side_;
};

} // namespace wa
