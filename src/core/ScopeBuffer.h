#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace wa {
class ScopeBuffer {
public:
    explicit ScopeBuffer(size_t capacitySamples) : buf_(capacitySamples ? capacitySamples : 1) {}
    void push(const float* mono, size_t n) {            // single producer
        const size_t cap = buf_.size();
        size_t w = (size_t)written_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) buf_[(w + i) % cap] = mono[i];
        written_.store(written_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }
    uint64_t totalWritten() const { return written_.load(std::memory_order_acquire); }
    bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const {
        const size_t cap = buf_.size();
        if (n == 0 || n > cap/2) return false;          // contract: n <= cap/2
        for (int attempt = 0; attempt < 8; ++attempt) {
            uint64_t w0 = written_.load(std::memory_order_acquire);
            if (w0 < n) return false;
            uint64_t start = w0 - n;
            for (size_t i = 0; i < n; ++i) out[i] = buf_[(size_t)((start + i) % cap)];
            uint64_t w1 = written_.load(std::memory_order_acquire);
            if (w1 - start <= cap) { endIdxOut = w0; return true; }  // window not overwritten during copy
        }
        return false;
    }
private:
    std::vector<float> buf_;
    std::atomic<uint64_t> written_{0};
};
} // namespace wa
