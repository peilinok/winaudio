#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "CaptureTrackList.h"

namespace wa {

inline constexpr uint32_t kRenderClientRate = 48000;
inline constexpr uint16_t kRenderClientBits = 16;

struct RenderLayout {
    uint16_t channels = 0;
    uint32_t channelMask = 0;
    std::string label;
};

struct RenderCustomLayout {
    bool ok = false;
    uint16_t channels = 0;
    uint32_t channelMask = 0;
    std::string message;
};

enum class RenderLayoutMode { SystemDefault, Layout };

// SystemDefault reads channel count and mask from source (the endpoint mix) and
// ignores its sample rate and bit depth. Layout uses source's channel count and
// mask as already chosen; rate and bit depth are ignored there too.
struct RenderTrackCreate {
    DeviceId deviceId;
    RenderLayoutMode mode = RenderLayoutMode::SystemDefault;
    AudioFormat source{};
};

struct RenderTrackStatus {
    TrackId id = 0;
    StreamState state = StreamState::Idle;
    DeviceId deviceId;
    AudioFormat clientFormat{};
    AudioFormat actualFormat{};
    std::vector<uint8_t> channelPaused; // 1 = Channel pause
    std::string message;
};

// One entry per distinct channel count and mask. Labels are channelLayoutLabel.
std::vector<RenderLayout> collapseSharedLayouts(const std::vector<AudioFormat>& candidates);

// Channel count 1..8 from a rate/bits/channels spec. Rate, bit depth, and float are ignored.
// Counts 1, 2, 4, 6, and 8 receive defaultChannelMask. Any other accepted count has mask 0.
RenderCustomLayout renderLayoutFromCustom(const std::string& text);

// Shared Render Tracks. Create starts immediately. Every channel starts paused.
// BackendFactory is the test seam (no WASAPI). An empty factory uses WASAPI Shared.
class RenderTrackList {
public:
    using BackendFactory =
        std::function<std::unique_ptr<IAudioBackend>(const DeviceId&, const AudioFormat&)>;

    explicit RenderTrackList(BackendFactory factory = {});
    ~RenderTrackList();

    RenderTrackList(const RenderTrackList&) = delete;
    RenderTrackList& operator=(const RenderTrackList&) = delete;

    // On failure the track stays listed with its own error. outId is set either way.
    Result create(const RenderTrackCreate& spec, TrackId* outId);
    void destroy(TrackId id);
    void destroyAll();
    std::vector<RenderTrackStatus> poll() const;

private:
    struct Member;
    Result adopt(std::unique_ptr<Member> member, Result result, TrackId* outId);

    BackendFactory factory_;
    TrackId nextId_ = 1;
    std::vector<std::unique_ptr<Member>> members_;
    mutable std::mutex mtx_;
};

} // namespace wa
