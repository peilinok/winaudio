#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wa {

// Single-producer / single-consumer lock-free byte ring buffer.
// write() is called from exactly one (audio) thread; read() from exactly one
// (engine) thread. Partial write increments overruns; partial read increments
// underruns. These counters feed the later latency/glitch phase.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacityBytes);

    size_t write(const void* data, size_t bytes);
    size_t read(void* out, size_t bytes);

    size_t capacity() const { return buf_.size(); }
    size_t availableRead() const;
    size_t availableWrite() const;

    uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }
    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

    void reset();

private:
    std::vector<uint8_t>  buf_;
    std::atomic<size_t>   head_{0};   // monotonic total bytes written
    std::atomic<size_t>   tail_{0};   // monotonic total bytes read
    std::atomic<uint64_t> overruns_{0};
    std::atomic<uint64_t> underruns_{0};
};

} // namespace wa
