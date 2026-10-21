#pragma once
#include <atomic>

namespace wa {

// Latched first runtime fault of a stream worker (an HRESULT; negative = failure).
// Written by the audio worker thread, read from any thread (pump, UI, poll).
// The first failure wins; later failures are ignored until reset(), which the
// owner calls before relaunching the worker. Success codes are never latched.
// Kept free of <windows.h> so it can be unit-tested without WASAPI.
class RuntimeFault {
public:
    // Returns true only for the call that latched the fault.
    bool report(long hr) noexcept {
        if (hr >= 0) return false;
        long expected = 0;
        return hr_.compare_exchange_strong(expected, hr, std::memory_order_acq_rel,
                                           std::memory_order_acquire);
    }
    bool failed() const noexcept { return hr_.load(std::memory_order_acquire) < 0; }
    long code() const noexcept { return hr_.load(std::memory_order_acquire); } // 0 = healthy
    void reset() noexcept { hr_.store(0, std::memory_order_release); }

private:
    std::atomic<long> hr_{0};
};

} // namespace wa
