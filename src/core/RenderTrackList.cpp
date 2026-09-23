#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RenderTrackList.h"
#include "AudioFormatStr.h"
#include "FormatSpec.h"
#include "Log.h"
#include "RingBuffer.h"
#include "WasapiStream.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace wa {

namespace {
constexpr size_t kRingBytes = 1u << 20;
constexpr uint32_t kQuantumFrames = 480;

AudioFormat clientFormat(uint16_t channels, uint32_t channelMask) {
    AudioFormat fmt;
    fmt.sampleRate = kRenderClientRate;
    fmt.bitsPerSample = kRenderClientBits;
    fmt.channels = channels;
    fmt.isFloat = false;
    fmt.channelMask = channelMask;
    return fmt;
}

const char* emptyLayoutMessage(RenderLayoutMode mode) {
    return mode == RenderLayoutMode::SystemDefault ? "render mix has no channels"
                                                   : "render layout has no channels";
}
} // namespace

std::vector<RenderLayout> collapseSharedLayouts(const std::vector<AudioFormat>& candidates) {
    std::vector<RenderLayout> out;
    for (const AudioFormat& fmt : candidates) {
        if (fmt.channels == 0) continue;
        const uint32_t mask = fmt.channelMask ? fmt.channelMask
                                              : defaultChannelMask(fmt.channels);
        bool seen = false;
        for (const RenderLayout& existing : out) {
            if (existing.channels == fmt.channels && existing.channelMask == mask) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        AudioFormat labeled = fmt;
        labeled.channelMask = mask;
        out.push_back(RenderLayout{fmt.channels, mask, channelLayoutLabel(labeled)});
    }
    return out;
}

RenderCustomLayout renderLayoutFromCustom(const std::string& text) {
    RenderCustomLayout out;
    AudioFormat parsed;
    if (!parseFormatSpec(text, parsed)) {
        out.message = "invalid format";
        return out;
    }
    out.ok = true;
    out.channels = parsed.channels;
    out.channelMask = defaultChannelMask(parsed.channels);
    return out;
}

struct RenderTrackList::Member {
    TrackId id = 0;
    uint16_t channels = 0;
    std::unique_ptr<IAudioBackend> backend;
    std::unique_ptr<RingBuffer> ring;
    std::vector<int16_t> scratch;
    std::thread pump;
    std::atomic<bool> running{false};
    RenderTrackStatus status{};

    ~Member() { stopPumpAndBackends(); }

    void stopPumpAndBackends() {
        running.store(false, std::memory_order_relaxed);
        if (pump.joinable()) pump.join();
        if (backend) {
            backend->stop();
            backend->close();
            backend.reset();
        }
        ring.reset();
    }
};

RenderTrackList::RenderTrackList(BackendFactory factory) : factory_(std::move(factory)) {}

RenderTrackList::~RenderTrackList() { destroyAll(); }

Result RenderTrackList::adopt(std::unique_ptr<Member> member, Result result, TrackId* outId) {
    std::lock_guard<std::mutex> lk(mtx_);
    member->id = nextId_++;
    member->status.id = member->id;
    if (outId) *outId = member->id;
    char mask[16]{};
    std::snprintf(mask, sizeof(mask), "0x%X",
                  static_cast<unsigned>(member->status.clientFormat.channelMask));
    const std::string dev = member->status.deviceId.empty()
                                ? "(default)"
                                : narrowAscii(member->status.deviceId);
    WA_LOG(result ? wa::log::Level::Info : wa::log::Level::Err, "RenderTrackList", "create",
           "id=" + std::to_string(member->id) + " dev=" + dev +
               " fmt=" + formatAudio(member->status.clientFormat) + " mask=" + mask,
           result ? "ok" : result.message);
    members_.push_back(std::move(member));
    return result;
}

Result RenderTrackList::create(const RenderTrackCreate& spec, TrackId* outId) {
    auto member = std::make_unique<Member>();
    member->status.deviceId = spec.deviceId;
    member->status.state = StreamState::Idle;

    if (spec.source.channels == 0) {
        const std::string message = emptyLayoutMessage(spec.mode);
        member->status.state = StreamState::Error;
        member->status.message = message;
        member->status.clientFormat.sampleRate = 0;
        member->status.clientFormat.bitsPerSample = 0;
        member->status.clientFormat.channels = 0;
        return adopt(std::move(member), Result::Fail(-1, message), outId);
    }

    const AudioFormat client = clientFormat(spec.source.channels, spec.source.channelMask);
    member->channels = client.channels;
    member->status.clientFormat = client;
    member->status.channelPaused.assign(client.channels, 1);

    try {
        member->scratch.assign(static_cast<size_t>(kQuantumFrames) * client.channels, 0);
        member->ring = std::make_unique<RingBuffer>(kRingBytes);
        if (factory_)
            member->backend = factory_(spec.deviceId, client);
        else
            member->backend = std::make_unique<WasapiRenderStream>(WasapiMode::Shared, &client);
        if (!member->backend) {
            const std::string message = "RenderTrackList: render factory returned null";
            member->status.state = StreamState::Error;
            member->status.message = message;
            return adopt(std::move(member), Result::Fail(-1, message), outId);
        }

        Result opened = member->backend->open(spec.deviceId, client, member->ring.get(), {});
        if (!opened) {
            const std::string message = opened.message.empty() ? "render open failed" : opened.message;
            member->backend->close();
            member->backend.reset();
            member->status.state = StreamState::Error;
            member->status.message = message;
            return adopt(std::move(member), Result::Fail(opened.code, message), outId);
        }
        Result started = member->backend->start();
        if (!started) {
            const std::string message = started.message.empty() ? "render start failed"
                                                                : started.message;
            member->backend->stop();
            member->backend->close();
            member->backend.reset();
            member->status.state = StreamState::Error;
            member->status.message = message;
            return adopt(std::move(member), Result::Fail(started.code, message), outId);
        }

        member->status.actualFormat = member->backend->stats().actualFormat;
        member->status.state = StreamState::Running;
        Member* raw = member.get();
        raw->running.store(true, std::memory_order_relaxed);
        raw->pump = std::thread([raw] {
            wa::log::setThreadName("renT");
            const size_t frameBytes = static_cast<size_t>(raw->channels) * sizeof(int16_t);
            while (raw->running.load(std::memory_order_relaxed)) {
                std::memset(raw->scratch.data(), 0, raw->scratch.size() * sizeof(int16_t));
                const size_t bytes = raw->scratch.size() * sizeof(int16_t);
                const size_t wrote = raw->ring->write(raw->scratch.data(), bytes);
                const unsigned frames =
                    frameBytes == 0 ? 0u : static_cast<unsigned>(wrote / frameBytes);
                wa::log::emitTrace("RenderTrackList", "write", frames, 0, 0);
                if (wrote < bytes) Sleep(5);
            }
        });
    } catch (const std::exception& e) {
        member->stopPumpAndBackends();
        const char* what = e.what();
        const std::string message = (what && what[0]) ? what : "render create failed";
        member->status.state = StreamState::Error;
        member->status.message = message;
        return adopt(std::move(member), Result::Fail(-1, message), outId);
    }

    return adopt(std::move(member), Result::Ok(), outId);
}

void RenderTrackList::destroy(TrackId id) {
    std::unique_ptr<Member> gone;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = std::find_if(members_.begin(), members_.end(),
                               [id](const std::unique_ptr<Member>& m) { return m->id == id; });
        if (it == members_.end()) return;
        gone = std::move(*it);
        members_.erase(it);
    }
    WA_LOG(wa::log::Level::Info, "RenderTrackList", "destroy", "id=" + std::to_string(id), "");
    gone->stopPumpAndBackends();
}

void RenderTrackList::destroyAll() {
    std::vector<std::unique_ptr<Member>> gone;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        gone.swap(members_);
    }
    WA_LOG(wa::log::Level::Info, "RenderTrackList", "destroyAll",
           "n=" + std::to_string(gone.size()), "");
    for (auto& m : gone) m->stopPumpAndBackends();
}

std::vector<RenderTrackStatus> RenderTrackList::poll() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<RenderTrackStatus> out;
    out.reserve(members_.size());
    for (const auto& m : members_) out.push_back(m->status);
    return out;
}

} // namespace wa
