#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace wa {
// SPSC seqlock ring buffer for display scope; concurrent float r/w relies on naturally-atomic aligned 32-bit access on x64.
class ScopeBuffer {
public:
    explicit ScopeBuffer(size_t capacitySamples) : ScopeBuffer(capacitySamples, 1) {}
    ScopeBuffer(size_t capacityFrames, uint16_t channels)
        : channels_(channels ? channels : 1),
          buf_((capacityFrames ? capacityFrames : 1) * static_cast<size_t>(channels_)) {}

    void push(const float* mono, size_t n) {            // single producer
        const size_t cap = capacityFrames();
        const uint64_t w = written_.load(std::memory_order_relaxed);
        // Seqlock: mark writing-in-progress (odd) before touching buf_, mark done (even) after.
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        for (size_t i = 0; i < n; ++i) {
            const size_t frame = static_cast<size_t>((w + i) % cap);
            for (uint16_t ch = 0; ch < channels_; ++ch)
                buf_[frame * static_cast<size_t>(channels_) + ch] = mono[i];
        }
        written_.store(w + n, std::memory_order_relaxed);
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    void pushInterleaved(const float* interleaved, size_t frames) {
        const size_t cap = capacityFrames();
        const uint64_t w = written_.load(std::memory_order_relaxed);
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        for (size_t i = 0; i < frames; ++i) {
            const size_t frame = static_cast<size_t>((w + i) % cap);
            for (uint16_t ch = 0; ch < channels_; ++ch) {
                buf_[frame * static_cast<size_t>(channels_) + ch] =
                    interleaved[i * static_cast<size_t>(channels_) + ch];
            }
        }
        written_.store(w + frames, std::memory_order_relaxed);
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    uint64_t totalWritten() const { return written_.load(std::memory_order_acquire); }
    uint16_t channels() const { return channels_; }

    // Consistent mono/downmix snapshot of the most recent n frames. Contract: n <= capacity/2.
    bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const {
        const size_t cap = capacityFrames();
        if (n == 0 || n > cap / 2) return false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint64_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1u) continue;                       // writer in progress -> retry
            const uint64_t w0 = written_.load(std::memory_order_acquire);
            if (w0 < n) return false;                    // not enough samples yet
            const uint64_t start = w0 - n;
            const float invCh = 1.0f / static_cast<float>(channels_);
            for (size_t i = 0; i < n; ++i) {
                const size_t frame = static_cast<size_t>((start + i) % cap);
                const float* src = &buf_[frame * static_cast<size_t>(channels_)];
                float sum = 0.0f;
                for (uint16_t ch = 0; ch < channels_; ++ch) sum += src[ch];
                out[i] = sum * invCh;
            }
            const uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s0 == s1) { endIdxOut = w0; return true; } // no write occurred during the copy
        }
        return false;                                    // couldn't get a quiet window (caller skips a frame)
    }

    bool snapshotLatestChannel(uint16_t channel, size_t n, float* out, uint64_t& endIdxOut) const {
        const size_t cap = capacityFrames();
        if (channel >= channels_ || n == 0 || n > cap / 2) return false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint64_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1u) continue;
            const uint64_t w0 = written_.load(std::memory_order_acquire);
            if (w0 < n) return false;
            const uint64_t start = w0 - n;
            for (size_t i = 0; i < n; ++i) {
                const size_t frame = static_cast<size_t>((start + i) % cap);
                out[i] = buf_[frame * static_cast<size_t>(channels_) + channel];
            }
            const uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s0 == s1) { endIdxOut = w0; return true; } // no write occurred during the copy
        }
        return false;                                    // couldn't get a quiet window (caller skips a frame)
    }

    bool snapshotChannelEndingAt(uint16_t channel, uint64_t endIdx, size_t n, float* out) const {
        const size_t cap = capacityFrames();
        if (channel >= channels_ || n == 0 || n > cap / 2) return false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint64_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1u) continue;
            const uint64_t w0 = written_.load(std::memory_order_acquire);
            if (!windowAvailable(w0, endIdx, n, cap)) return false;
            const uint64_t start = endIdx - n;
            for (size_t i = 0; i < n; ++i) {
                const size_t frame = static_cast<size_t>((start + i) % cap);
                out[i] = buf_[frame * static_cast<size_t>(channels_) + channel];
            }
            const uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s0 == s1) return true;                   // no write occurred during the copy
        }
        return false;                                    // couldn't get a quiet window (caller skips a frame)
    }

private:
    size_t capacityFrames() const { return buf_.size() / static_cast<size_t>(channels_); }
    static bool windowAvailable(uint64_t written, uint64_t endIdx, size_t n, size_t cap) {
        if (endIdx > written || endIdx < static_cast<uint64_t>(n)) return false;
        const uint64_t start = endIdx - static_cast<uint64_t>(n);
        return written - start <= static_cast<uint64_t>(cap);
    }
    uint16_t channels_ = 1;
    std::vector<float> buf_;
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> seq_{0};
};
} // namespace wa
