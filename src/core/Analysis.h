#pragma once
#include <cstddef>
#include <cstdint>
namespace wa {
// Advance analysis by fixed sample-count hops, decoupled from caller frame rate.
// nextEndIdx==0 means "not started"; it is pre-seeded to (windowSize - hop) so the
// first emit lands exactly at windowSize. Between calls, nextEndIdx holds the hop
// boundary AFTER the last processed frame (i.e. the next frame to emit is at nextEndIdx).
// If more than maxCatchup hops are pending (caller fell far behind), FAST-FORWARD
// nextEndIdx to skip stale hops, so it never spins through a huge backlog.
// Returns the number of frames processed (onFrame calls).
template <class Fn>
size_t advanceAnalysis(uint64_t written, uint64_t& nextEndIdx, size_t windowSize, size_t hop,
                       size_t maxCatchup, Fn&& onFrame) {
    if (hop == 0) return 0;
    const uint64_t uHop = static_cast<uint64_t>(hop);
    // Initialize on first use: pre-seed so the first advance-then-emit lands at windowSize.
    if (nextEndIdx == 0) {
        nextEndIdx = static_cast<uint64_t>(windowSize) - uHop;
    }
    // Nothing to do if the next hop boundary is beyond written.
    if (written < nextEndIdx + uHop) return 0;
    // Count pending hops.
    uint64_t pending = (written - nextEndIdx) / uHop;
    if (pending > static_cast<uint64_t>(maxCatchup)) {
        // Fast-forward: skip stale, keep only the most recent maxCatchup hops.
        uint64_t skip = pending - static_cast<uint64_t>(maxCatchup);
        nextEndIdx += skip * uHop;
    }
    size_t processed = 0;
    while (nextEndIdx + uHop <= written) {
        nextEndIdx += uHop;        // advance first
        onFrame(nextEndIdx);       // emit at new boundary
        ++processed;
    }
    // Advance past the last processed frame so the next call resumes correctly.
    nextEndIdx += uHop;
    return processed;
}
} // namespace wa
