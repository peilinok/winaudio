#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "Engine.h"
#include "IAudioBackend.h"
#include "MonitorEngine.h"
#include "Result.h"

namespace wa {

using TrackId = uint64_t;

struct CaptureTrackCreate {
    BackendKind     kind = BackendKind::WasapiShared;
    CaptureSource   source{};
    std::wstring    wavPath;
    const AudioFormat* requested = nullptr;
    LoopbackOptions loopbackOptions{};
};

struct CaptureTrackStatus {
    TrackId       id = 0;
    StreamState   state = StreamState::Idle;
    CaptureSource source{};
    AudioFormat   actualFormat{};
    float         levelL = 0.f;
    float         levelR = 0.f;
    uint64_t      overruns = 0;
    std::string   message;
};

// Live Capture Tracks with independent lifetime. Create starts immediately.
// BackendFactory / SilentRenderFactory are test seams (no WASAPI).
class CaptureTrackList {
public:
    using BackendFactory =
        std::function<std::unique_ptr<IAudioBackend>(const CaptureSource&, const AudioFormat*)>;
    using SilentRenderFactory =
        std::function<std::unique_ptr<IAudioBackend>(const AudioFormat*)>;

    explicit CaptureTrackList(BackendFactory factory = {}, SilentRenderFactory silentFactory = {});
    ~CaptureTrackList();

    CaptureTrackList(const CaptureTrackList&)            = delete;
    CaptureTrackList& operator=(const CaptureTrackList&) = delete;

    Result create(const CaptureTrackCreate& spec, TrackId* outId);
    void   destroy(TrackId id);
    void   destroyAll();
    std::vector<CaptureTrackStatus> poll() const;

private:
    struct Member;
    BackendFactory       factory_;
    SilentRenderFactory  silentFactory_;
    TrackId              nextId_ = 1;
    std::vector<std::unique_ptr<Member>> members_;
    mutable std::mutex   mtx_;
};

} // namespace wa
