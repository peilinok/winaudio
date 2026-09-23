#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "CaptureTrackList.h"
#include "PhraseCatalog.h"

namespace wa {

// Test backends pull interleaved frames through this. Production does not.
struct RenderFill {
    void (*fn)(void* ctx, int16_t* interleaved, uint32_t frames) = nullptr;
    void* ctx = nullptr;
};

struct RenderChannelStatus {
    uint8_t paused = 1;
    uint8_t volumePercent = 100;
    uint8_t hasPhrase = 0;
    ChannelPhrase phrase = ChannelPhrase::None;
};

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
    std::vector<RenderChannelStatus> channels;
    uint16_t chartChannels = 0; // first eight, for the Chart Host tap
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
    using BackendFactory = std::function<std::unique_ptr<IAudioBackend>(
        const DeviceId&, const AudioFormat&, RenderFill*)>;

    // catalog may be null. It is read only during create; the track copies the clips.
    explicit RenderTrackList(BackendFactory factory = {}, const PhraseCatalog* catalog = nullptr);
    ~RenderTrackList();

    RenderTrackList(const RenderTrackList&) = delete;
    RenderTrackList& operator=(const RenderTrackList&) = delete;

    // On failure the track stays listed with its own error. outId is set either way.
    Result create(const RenderTrackCreate& spec, TrackId* outId);
    void destroy(TrackId id);
    void destroyAll();
    std::vector<RenderTrackStatus> poll() const;

    Result continueChannel(TrackId id, uint16_t channel);
    Result pauseChannel(TrackId id, uint16_t channel);
    Result playOnce(TrackId id, uint16_t channel);
    Result setVolume(TrackId id, uint16_t channel, int percent);

    uint64_t written(TrackId id) const;
    uint16_t tapChannels(TrackId id) const;
    bool snapshotLatest(TrackId id, size_t n, float* out, uint64_t& endIdxOut) const;
    bool snapshotEndingAt(TrackId id, uint64_t endIdx, size_t n, float* out) const;
    bool snapshotChannelEndingAt(TrackId id, uint16_t channel, uint64_t endIdx, size_t n,
                                 float* out) const;

private:
    struct Member;
    struct Voice;
    Result adopt(std::unique_ptr<Member> member, Result result, TrackId* outId);
    Member* findUnlocked(TrackId id);
    const Member* findUnlocked(TrackId id) const;
    Voice* voiceUnlocked(TrackId id, uint16_t channel);

    BackendFactory factory_;
    const PhraseCatalog* catalog_ = nullptr;
    TrackId nextId_ = 1;
    std::vector<std::unique_ptr<Member>> members_;
    mutable std::mutex mtx_;
};

} // namespace wa
