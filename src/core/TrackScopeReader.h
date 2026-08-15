#pragma once
#include "CaptureTrackList.h"
#include "ScopeReader.h"

namespace wa {

class TrackScopeReader final : public ScopeReader {
public:
    TrackScopeReader(const CaptureTrackList& list, TrackId id) : list_(list), id_(id) {}

    uint64_t written() const override { return list_.written(id_); }
    uint16_t channels() const override { return list_.tapChannels(id_); }

    bool snapshotLatest(size_t n, float* out, uint64_t& endIdxOut) const override {
        return list_.snapshotLatest(id_, n, out, endIdxOut);
    }
    bool snapshotEndingAt(uint64_t endIdx, size_t n, float* out) const override {
        return list_.snapshotEndingAt(id_, endIdx, n, out);
    }
    bool snapshotChannelEndingAt(uint16_t channel, uint64_t endIdx, size_t n,
                                 float* out) const override {
        return list_.snapshotChannelEndingAt(id_, channel, endIdx, n, out);
    }

private:
    const CaptureTrackList& list_;
    TrackId                 id_ = 0;
};

} // namespace wa
