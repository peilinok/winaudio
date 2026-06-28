#include "RingBuffer.h"
#include <algorithm>
#include <cstring>

namespace wa {

RingBuffer::RingBuffer(size_t capacityBytes)
    : buf_(capacityBytes == 0 ? 1 : capacityBytes) {}

size_t RingBuffer::availableRead() const {
    return head_.load(std::memory_order_acquire) -
           tail_.load(std::memory_order_acquire);
}

size_t RingBuffer::availableWrite() const {
    return buf_.size() - availableRead();
}

size_t RingBuffer::write(const void* data, size_t bytes) {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    const size_t freeBytes = buf_.size() - (h - t);
    const size_t n = std::min(bytes, freeBytes);
    if (n < bytes) overruns_.fetch_add(1, std::memory_order_relaxed);

    const size_t cap = buf_.size();
    const size_t off = h % cap;
    const size_t first = std::min(n, cap - off);
    std::memcpy(buf_.data() + off, data, first);
    if (n > first)
        std::memcpy(buf_.data(), static_cast<const uint8_t*>(data) + first, n - first);

    head_.store(h + n, std::memory_order_release);
    return n;
}

size_t RingBuffer::read(void* out, size_t bytes) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t avail = h - t;
    const size_t n = std::min(bytes, avail);
    if (n < bytes) underruns_.fetch_add(1, std::memory_order_relaxed);

    const size_t cap = buf_.size();
    const size_t off = t % cap;
    const size_t first = std::min(n, cap - off);
    std::memcpy(out, buf_.data() + off, first);
    if (n > first)
        std::memcpy(static_cast<uint8_t*>(out) + first, buf_.data(), n - first);

    tail_.store(t + n, std::memory_order_release);
    return n;
}

void RingBuffer::reset() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
}

} // namespace wa
