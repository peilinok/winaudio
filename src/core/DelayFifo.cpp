#include "DelayFifo.h"
#include <algorithm>

namespace wa {

DelayFifo::DelayFifo(size_t channels, size_t targetFrames,
                     size_t capacityFrames, size_t deadbandFrames)
    : channels_(channels)
    , targetFrames_(targetFrames)
    , capacityFrames_(capacityFrames)
    , deadbandFrames_(deadbandFrames)
    , buf_(capacityFrames * channels, 0.f)
    , lowpassFill_(static_cast<double>(targetFrames))
    , lastFrame_(channels, 0.f)
{}

size_t   DelayFifo::fillFrames()        const { return fill_; }
double   DelayFifo::lowpassFillFrames() const { return lowpassFill_; }
uint64_t DelayFifo::driftFixes()        const { return driftFixes_; }

void DelayFifo::pushFrames(const float* interleaved, size_t frames) {
    if (frames == 0) return;

    // If input exceeds capacity, keep only the most recent capacityFrames_ frames.
    if (frames > capacityFrames_) {
        interleaved += (frames - capacityFrames_) * channels_;
        frames = capacityFrames_;
    }

    // Drop oldest frames if buffer would overflow.
    if (fill_ + frames > capacityFrames_) {
        size_t drop = fill_ + frames - capacityFrames_;
        head_ = (head_ + drop) % capacityFrames_;
        fill_ -= drop;
    }

    // Write frames into ring.
    for (size_t i = 0; i < frames; ++i) {
        float*       dst = buf_.data() + tail_ * channels_;
        const float* src = interleaved  + i    * channels_;
        for (size_t c = 0; c < channels_; ++c)
            dst[c] = src[c];
        tail_ = (tail_ + 1) % capacityFrames_;
    }
    fill_ += frames;
}

size_t DelayFifo::popFrames(float* out, size_t maxFrames) {
    if (maxFrames == 0) return 0;

    // 1. Update EMA.
    lowpassFill_ += kAlpha * (static_cast<double>(fill_) - lowpassFill_);

    size_t written = 0;

    double hi = static_cast<double>(targetFrames_ + deadbandFrames_);
    double lo = (deadbandFrames_ < targetFrames_)
                    ? static_cast<double>(targetFrames_ - deadbandFrames_)
                    : 0.0;

    // 2. Drift controller — single-frame granularity.
    if (lowpassFill_ > hi && fill_ > 0) {
        // DROP: skip one frame, then crossfade from lastFrame_ into the ring.
        ++driftFixes_;

        // Advance past the dropped frame.
        head_ = (head_ + 1) % capacityFrames_;
        --fill_;

        // Crossfade: blend lastFrame_ toward current ring frames over xLen steps.
        // lastFrame_ is only updated after this loop, so read it directly (no per-DROP copy).
        size_t xLen = std::min({kCrossFadeFrames, fill_, maxFrames});
        for (size_t i = 0; i < xLen; ++i) {
            float t = static_cast<float>(i + 1) / static_cast<float>(xLen + 1);
            float*       dst = out + written * channels_;
            const float* rp  = buf_.data() + head_ * channels_;
            for (size_t c = 0; c < channels_; ++c)
                dst[c] = lastFrame_[c] * (1.f - t) + rp[c] * t;
            head_ = (head_ + 1) % capacityFrames_;
            --fill_;
            ++written;
        }
        // Track last output frame.
        if (written > 0) {
            const float* lastOut = out + (written - 1) * channels_;
            for (size_t c = 0; c < channels_; ++c)
                lastFrame_[c] = lastOut[c];
        }

    } else if (lowpassFill_ < lo && fill_ > 0) {
        // DUP: insert one blended frame without consuming ring data.
        ++driftFixes_;
        if (written < maxFrames) {
            float*       dst = out + written * channels_;
            const float* rp  = buf_.data() + head_ * channels_;
            for (size_t c = 0; c < channels_; ++c) {
                dst[c] = lastFrame_[c] * 0.5f + rp[c] * 0.5f;
                lastFrame_[c] = dst[c];
            }
            ++written;
        }
    }

    // 3. Copy remaining frames from ring into output.
    size_t toCopy = std::min(maxFrames - written, fill_);
    for (size_t i = 0; i < toCopy; ++i) {
        float*       dst = out  + written * channels_;
        const float* src = buf_.data() + head_ * channels_;
        for (size_t c = 0; c < channels_; ++c)
            dst[c] = src[c];
        for (size_t c = 0; c < channels_; ++c)
            lastFrame_[c] = dst[c];
        head_ = (head_ + 1) % capacityFrames_;
        --fill_;
        ++written;
    }

    return written;
}

} // namespace wa
