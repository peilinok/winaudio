#pragma once
#include "ScopeReader.h"
#include <cstdint>
#include <vector>

namespace wa {

// In-memory Scope Reader for Chart Data Pipeline / hop tests (no audio hardware).
// Stores interleaved frames [ch0, ch1, ...] chronologically from index 0.
class FakeScopeReader final : public ScopeReader {
public:
    explicit FakeScopeReader(uint16_t channels = 1)
        : channels_(channels ? channels : static_cast<uint16_t>(1)) {}

    // Append frames of interleaved samples (size must be frames * channels).
    void pushInterleaved(const float* interleaved, size_t frames) {
        if (!interleaved || frames == 0) return;
        const size_t n = frames * static_cast<size_t>(channels_);
        samples_.insert(samples_.end(), interleaved, interleaved + n);
        written_ += frames;
    }

    // Append mono samples (broadcast to all channels if multi-ch).
    void pushMono(const float* mono, size_t frames) {
        if (!mono || frames == 0) return;
        for (size_t i = 0; i < frames; ++i) {
            for (uint16_t ch = 0; ch < channels_; ++ch)
                samples_.push_back(mono[i]);
        }
        written_ += frames;
    }

    // Set absolute written count without growing samples (advanced tests).
    void setWritten(uint64_t w) { written_ = w; }

    uint64_t written() const override { return written_; }
    uint16_t channels() const override { return channels_; }

    bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const override {
        if (!out || n == 0 || written_ < n) return false;
        const uint64_t end = written_;
        if (!snapshotEndingAt(end, n, out)) return false;
        endIdxOut = end;
        return true;
    }

    bool snapshotEndingAt(uint64_t endIdx, size_t n, float* out) const override {
        if (!out || n == 0 || endIdx > written_ || endIdx < n) return false;
        const uint64_t start = endIdx - n;
        // Only frames we actually stored (from 0) are available.
        if (endIdx > static_cast<uint64_t>(samples_.size() / channels_)) return false;
        const float invCh = 1.0f / static_cast<float>(channels_);
        for (size_t i = 0; i < n; ++i) {
            const size_t frame = static_cast<size_t>(start + i);
            float sum = 0.0f;
            for (uint16_t ch = 0; ch < channels_; ++ch)
                sum += samples_[frame * static_cast<size_t>(channels_) + ch];
            out[i] = sum * invCh;
        }
        return true;
    }

    bool snapshotChannelEndingAt(uint16_t channel, uint64_t endIdx, size_t n,
                                 float* out) const override {
        if (!out || channel >= channels_ || n == 0 || endIdx > written_ || endIdx < n)
            return false;
        if (endIdx > static_cast<uint64_t>(samples_.size() / channels_)) return false;
        const uint64_t start = endIdx - n;
        for (size_t i = 0; i < n; ++i) {
            const size_t frame = static_cast<size_t>(start + i);
            out[i] = samples_[frame * static_cast<size_t>(channels_) + channel];
        }
        return true;
    }

private:
    uint16_t           channels_ = 1;
    uint64_t           written_  = 0;
    std::vector<float> samples_;
};

} // namespace wa
