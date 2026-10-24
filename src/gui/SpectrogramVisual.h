#pragma once
#include "Spectrogram.h"
#include "HeatmapTexture.h"
#include <memory>
#include <utility>

namespace wa {
// One history and its lazily-created GPU resource. Replacement invalidates both,
// including a new history with the same dimensions/revision as its predecessor.
class SpectrogramVisual {
public:
    Spectrogram* get() const { return history_.get(); }
    explicit operator bool() const { return history_ != nullptr; }
    void reset(std::unique_ptr<Spectrogram> history = {}) {
        texture_.reset();
        history_ = std::move(history);
    }
    void releaseGpu() { texture_.reset(); }
    HeatmapTexture* ensure(const uint32_t* lut, FrameProbe* probe = nullptr,
                           uint64_t source = 0, int direction = -1, int channel = -1) {
        if (!history_) return nullptr;
        if (!texture_) texture_ = std::make_unique<HeatmapTexture>();
        return texture_->ensure(history_->data(), history_->rows(), history_->cols(),
            history_->revision(), -96.f, 0.f, lut, probe, source, direction, channel)
            ? texture_.get() : nullptr;
    }
private:
    std::unique_ptr<Spectrogram> history_;
    std::unique_ptr<HeatmapTexture> texture_;
};
}
