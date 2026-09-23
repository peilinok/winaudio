#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RenderTrackList.h"
#include "AudioFormatStr.h"
#include "FormatSpec.h"
#include "Log.h"
#include "RingBuffer.h"
#include "ScopeBuffer.h"
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
// One quantum queued, not the whole ring, so pause / volume / play-once
// reach the device within 10 ms at 48 kHz.
constexpr uint32_t kMaxQueuedFrames = kQuantumFrames;
constexpr uint16_t kChartChannelCap = 8;

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

int16_t scaledSample(int16_t sample, uint8_t volumePercent) {
    return static_cast<int16_t>(static_cast<int32_t>(sample) * static_cast<int32_t>(volumePercent) /
                                100);
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

struct RenderTrackList::Voice {
    ChannelPhrase phrase = ChannelPhrase::None;
    std::vector<int16_t> pcm;
    std::atomic<uint8_t> paused{1};
    std::atomic<uint8_t> volume{100};
    std::atomic<uint32_t> restartGen{0};
    std::atomic<uint32_t> oneShotGen{0};
    std::atomic<uint32_t> cancelGen{0};
    uint32_t cursor = 0;
    uint32_t seenRestart = 0;
    uint32_t seenOneShot = 0;
    uint32_t seenCancel = 0;
    bool oneShot = false;
};

struct RenderTrackList::Member {
    TrackId id = 0;
    uint16_t channels = 0;
    uint16_t tapChannels = 0;
    std::unique_ptr<Voice[]> voices;
    std::unique_ptr<IAudioBackend> backend;
    std::unique_ptr<RingBuffer> ring;
    std::unique_ptr<ScopeBuffer> tap;
    std::vector<int16_t> scratch;
    std::vector<float> scopeScratch;
    std::thread pump;
    std::atomic<bool> running{false};
    RenderFill fill{};
    RenderTrackStatus status{};

    ~Member() { stopPumpAndBackends(); }

    void noteCommands(Voice& voice);
    int16_t nextSample(Voice& voice);
    void renderChunk(int16_t* dst, uint32_t frames);
    void renderFrames(int16_t* dst, uint32_t frames);
    static void fillThunk(void* ctx, int16_t* dst, uint32_t frames);

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

void RenderTrackList::Member::noteCommands(Voice& voice) {
    const uint32_t cancel = voice.cancelGen.load(std::memory_order_acquire);
    if (cancel != voice.seenCancel) {
        voice.seenCancel = cancel;
        voice.oneShot = false;
    }
    const uint32_t restart = voice.restartGen.load(std::memory_order_acquire);
    if (restart != voice.seenRestart) {
        voice.seenRestart = restart;
        voice.cursor = 0;
        const uint32_t oneShot = voice.oneShotGen.load(std::memory_order_acquire);
        if (oneShot != voice.seenOneShot) {
            voice.seenOneShot = oneShot;
            voice.oneShot = voice.paused.load(std::memory_order_acquire) != 0;
        }
    }
}

int16_t RenderTrackList::Member::nextSample(Voice& voice) {
    noteCommands(voice);
    const bool paused = voice.paused.load(std::memory_order_acquire) != 0;
    if ((paused && !voice.oneShot) || voice.pcm.empty()) return 0;
    const uint32_t n = static_cast<uint32_t>(voice.pcm.size());
    if (voice.cursor >= n) {
        if (voice.oneShot) {
            voice.oneShot = false;
            voice.cursor = 0;
            return 0;
        }
        voice.cursor = 0;
    }
    const int16_t sample =
        scaledSample(voice.pcm[voice.cursor], voice.volume.load(std::memory_order_acquire));
    ++voice.cursor;
    if (voice.cursor >= n && voice.oneShot) {
        voice.oneShot = false;
        voice.cursor = 0;
    }
    return sample;
}

void RenderTrackList::Member::renderChunk(int16_t* dst, uint32_t frames) {
    const uint16_t ch = channels;
    const uint16_t tapCh = tapChannels;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint16_t channel = 0; channel < ch; ++channel) {
            const int16_t sample = voices ? nextSample(voices[channel]) : static_cast<int16_t>(0);
            dst[static_cast<size_t>(frame) * ch + channel] = sample;
            if (channel < tapCh) {
                scopeScratch[static_cast<size_t>(frame) * tapCh + channel] =
                    static_cast<float>(sample) / 32768.f;
            }
        }
    }
    if (tap && tapCh > 0) tap->pushInterleaved(scopeScratch.data(), frames);
}

void RenderTrackList::Member::renderFrames(int16_t* dst, uint32_t frames) {
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(kQuantumFrames, frames - done);
        renderChunk(dst + static_cast<size_t>(done) * channels, n);
        done += n;
    }
}

void RenderTrackList::Member::fillThunk(void* ctx, int16_t* dst, uint32_t frames) {
    static_cast<Member*>(ctx)->renderFrames(dst, frames);
}

RenderTrackList::RenderTrackList(BackendFactory factory, const PhraseCatalog* catalog)
    : factory_(std::move(factory)), catalog_(catalog) {}

RenderTrackList::~RenderTrackList() { destroyAll(); }

RenderTrackList::Member* RenderTrackList::findUnlocked(TrackId id) {
    for (auto& member : members_) {
        if (member->id == id) return member.get();
    }
    return nullptr;
}

const RenderTrackList::Member* RenderTrackList::findUnlocked(TrackId id) const {
    for (const auto& member : members_) {
        if (member->id == id) return member.get();
    }
    return nullptr;
}

RenderTrackList::Voice* RenderTrackList::voiceUnlocked(TrackId id, uint16_t channel) {
    Member* member = findUnlocked(id);
    if (!member || !member->voices || channel >= member->channels) return nullptr;
    return &member->voices[channel];
}

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
    member->tapChannels = std::min(client.channels, kChartChannelCap);
    member->status.clientFormat = client;
    member->status.chartChannels = member->tapChannels;
    member->voices = std::make_unique<Voice[]>(member->channels);
    for (uint16_t channel = 0; channel < member->channels; ++channel) {
        Voice& voice = member->voices[channel];
        voice.paused.store(1, std::memory_order_relaxed);
        voice.volume.store(100, std::memory_order_relaxed);
        voice.phrase = phraseForSlot(client.channelMask, channel);
        if (catalog_ && voice.phrase != ChannelPhrase::None) {
            if (const std::vector<int16_t>* clip = catalog_->find(voice.phrase))
                voice.pcm = *clip;
        }
    }

    try {
        member->scratch.assign(static_cast<size_t>(kQuantumFrames) * client.channels, 0);
        member->scopeScratch.assign(static_cast<size_t>(kQuantumFrames) * member->tapChannels, 0.f);
        member->ring = std::make_unique<RingBuffer>(kRingBytes);
        const size_t scopeFrames = std::max<size_t>(static_cast<size_t>(kRenderClientRate) * 2u,
                                                   1048576u);
        member->tap = std::make_unique<ScopeBuffer>(scopeFrames, member->tapChannels);
        member->fill.fn = &Member::fillThunk;
        member->fill.ctx = member.get();
        if (factory_)
            member->backend = factory_(spec.deviceId, client, &member->fill);
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
        if (!factory_) {
            Member* raw = member.get();
            raw->running.store(true, std::memory_order_relaxed);
            raw->pump = std::thread([raw] {
                wa::log::setThreadName("renT");
                const size_t frameBytes = static_cast<size_t>(raw->channels) * sizeof(int16_t);
                while (raw->running.load(std::memory_order_relaxed)) {
                    const uint32_t queued = frameBytes == 0
                                                ? 0u
                                                : static_cast<uint32_t>(raw->ring->availableRead() /
                                                                        frameBytes);
                    const uint32_t space = frameBytes == 0
                                               ? 0u
                                               : static_cast<uint32_t>(raw->ring->availableWrite() /
                                                                       frameBytes);
                    if (queued >= kMaxQueuedFrames || space == 0) {
                        Sleep(5);
                        continue;
                    }
                    const uint32_t room = kMaxQueuedFrames - queued;
                    const uint32_t frames = std::min(kQuantumFrames, std::min(space, room));
                    raw->renderChunk(raw->scratch.data(), frames);
                    const size_t bytes = static_cast<size_t>(frames) * frameBytes;
                    const size_t wrote = raw->ring->write(raw->scratch.data(), bytes);
                    const unsigned writtenFrames =
                        frameBytes == 0 ? 0u : static_cast<unsigned>(wrote / frameBytes);
                    wa::log::emitTrace("RenderTrackList", "write", writtenFrames, 0, 0);
                    if (wrote < bytes) Sleep(5);
                }
            });
        }
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
    for (const auto& member : members_) {
        RenderTrackStatus status = member->status;
        status.chartChannels = member->tapChannels;
        status.channelPaused.assign(member->channels, 1);
        status.channels.assign(member->channels, {});
        if (member->voices) {
            for (uint16_t channel = 0; channel < member->channels; ++channel) {
                const Voice& voice = member->voices[channel];
                const uint8_t paused = voice.paused.load(std::memory_order_acquire);
                status.channelPaused[channel] = paused;
                status.channels[channel].paused = paused;
                status.channels[channel].volumePercent =
                    voice.volume.load(std::memory_order_acquire);
                status.channels[channel].phrase = voice.phrase;
                status.channels[channel].hasPhrase = voice.pcm.empty() ? 0 : 1;
            }
        }
        out.push_back(std::move(status));
    }
    return out;
}

Result RenderTrackList::continueChannel(TrackId id, uint16_t channel) {
    std::lock_guard<std::mutex> lk(mtx_);
    Voice* voice = voiceUnlocked(id, channel);
    if (!voice) return Result::Fail(-1, "render channel not found");
    voice->paused.store(0, std::memory_order_release);
    voice->cancelGen.fetch_add(1, std::memory_order_release);
    WA_LOG(wa::log::Level::Debug, "RenderTrackList", "continue",
           "id=" + std::to_string(id) + " ch=" + std::to_string(channel), "");
    return Result::Ok();
}

Result RenderTrackList::pauseChannel(TrackId id, uint16_t channel) {
    std::lock_guard<std::mutex> lk(mtx_);
    Voice* voice = voiceUnlocked(id, channel);
    if (!voice) return Result::Fail(-1, "render channel not found");
    voice->paused.store(1, std::memory_order_release);
    voice->cancelGen.fetch_add(1, std::memory_order_release);
    WA_LOG(wa::log::Level::Debug, "RenderTrackList", "pause",
           "id=" + std::to_string(id) + " ch=" + std::to_string(channel), "");
    return Result::Ok();
}

Result RenderTrackList::playOnce(TrackId id, uint16_t channel) {
    std::lock_guard<std::mutex> lk(mtx_);
    Voice* voice = voiceUnlocked(id, channel);
    if (!voice) return Result::Fail(-1, "render channel not found");
    if (voice->paused.load(std::memory_order_acquire) != 0)
        voice->oneShotGen.fetch_add(1, std::memory_order_release);
    voice->restartGen.fetch_add(1, std::memory_order_release);
    WA_LOG(wa::log::Level::Debug, "RenderTrackList", "playOnce",
           "id=" + std::to_string(id) + " ch=" + std::to_string(channel), "");
    return Result::Ok();
}

Result RenderTrackList::setVolume(TrackId id, uint16_t channel, int percent) {
    std::lock_guard<std::mutex> lk(mtx_);
    Voice* voice = voiceUnlocked(id, channel);
    if (!voice) return Result::Fail(-1, "render channel not found");
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    voice->volume.store(static_cast<uint8_t>(percent), std::memory_order_release);
    WA_LOG(wa::log::Level::Debug, "RenderTrackList", "setVolume",
           "id=" + std::to_string(id) + " ch=" + std::to_string(channel) +
               " pct=" + std::to_string(percent),
           "");
    return Result::Ok();
}

uint64_t RenderTrackList::written(TrackId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* member = findUnlocked(id);
    return (member && member->tap) ? member->tap->totalWritten() : 0;
}

uint16_t RenderTrackList::tapChannels(TrackId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* member = findUnlocked(id);
    return member ? member->tapChannels : 0;
}

bool RenderTrackList::snapshotLatest(TrackId id, size_t n, float* out, uint64_t& endIdxOut) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* member = findUnlocked(id);
    return (member && member->tap) ? member->tap->snapshotLatest(n, out, endIdxOut) : false;
}

bool RenderTrackList::snapshotEndingAt(TrackId id, uint64_t endIdx, size_t n, float* out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* member = findUnlocked(id);
    return (member && member->tap) ? member->tap->snapshotEndingAt(endIdx, n, out) : false;
}

bool RenderTrackList::snapshotChannelEndingAt(TrackId id, uint16_t channel, uint64_t endIdx,
                                              size_t n, float* out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const Member* member = findUnlocked(id);
    return (member && member->tap)
               ? member->tap->snapshotChannelEndingAt(channel, endIdx, n, out)
               : false;
}

} // namespace wa
