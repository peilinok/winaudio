#pragma once
#include <cstdint>

namespace wa::capture_channel_view {

constexpr uint32_t kMaxCaptureChannelsShown = 8;

struct Plan {
    uint32_t actualChannels = 1;
    uint32_t visibleChannels = 1;
    bool split = false;
    bool truncated = false;
};

inline Plan makePlan(uint32_t actualCaptureChannels) {
    const uint32_t actual = actualCaptureChannels > 0 ? actualCaptureChannels : 1u;
    const uint32_t visible = actual < kMaxCaptureChannelsShown
                                 ? actual
                                 : kMaxCaptureChannelsShown;
    return Plan{actual, visible, actual > 1u, actual > visible};
}

} // namespace wa::capture_channel_view
