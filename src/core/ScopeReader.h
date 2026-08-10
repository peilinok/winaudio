#pragma once
#include <cstddef>
#include <cstdint>

namespace wa {

// Narrow scope-tap read surface for Chart Data Pipeline (one stream side).
// Production adapter: MonitorScopeReader. Tests: FakeScopeReader.
class ScopeReader {
public:
    virtual ~ScopeReader() = default;

    // Total frames written to the tap (exclusive end index of the full history).
    virtual uint64_t written() const = 0;

    // Channel count stored in the tap (1 for mono/render; capture may be multi-ch).
    virtual uint16_t channels() const = 0;

    // Mono/downmix of the most recent n frames; endIdxOut is exclusive end on success.
    virtual bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const = 0;

    // Mono/downmix of n frames ending at endIdx (exclusive).
    virtual bool snapshotEndingAt(uint64_t endIdx, size_t n, float* out) const = 0;

    // Single channel of n frames ending at endIdx (exclusive).
    virtual bool snapshotChannelEndingAt(uint16_t channel, uint64_t endIdx, size_t n,
                                         float* out) const = 0;
};

} // namespace wa
