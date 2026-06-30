#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace wa {
// SPSC seqlock ring buffer for display scope; concurrent float r/w relies on naturally-atomic aligned 32-bit access on x64.
class ScopeBuffer {
public:
    explicit ScopeBuffer(size_t capacitySamples) : buf_(capacitySamples ? capacitySamples : 1) {}

    void push(const float* mono, size_t n) {            // single producer
        const size_t cap = buf_.size();
        const uint64_t w = written_.load(std::memory_order_relaxed);
        // Seqlock: mark writing-in-progress (odd) before touching buf_, mark done (even) after.
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        for (size_t i = 0; i < n; ++i) buf_[(size_t)((w + i) % cap)] = mono[i];
        written_.store(w + n, std::memory_order_relaxed);
        seq_.store(seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    uint64_t totalWritten() const { return written_.load(std::memory_order_acquire); }

    // Consistent snapshot of the most recent n samples. Contract: n <= capacity/2.
    bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const {
        const size_t cap = buf_.size();
        if (n == 0 || n > cap / 2) return false;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const uint64_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1u) continue;                       // writer in progress -> retry
            const uint64_t w0 = written_.load(std::memory_order_acquire);
            if (w0 < n) return false;                    // not enough samples yet
            const uint64_t start = w0 - n;
            for (size_t i = 0; i < n; ++i) out[i] = buf_[(size_t)((start + i) % cap)];
            const uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s0 == s1) { endIdxOut = w0; return true; } // no write occurred during the copy
        }
        return false;                                    // couldn't get a quiet window (caller skips a frame)
    }

private:
    std::vector<float> buf_;
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> seq_{0};
};
} // namespace wa
