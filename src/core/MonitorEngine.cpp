// MonitorEngine.cpp - dual-stream delayed monitor pass-through.
//
// Threading model (spec 3 / 4.5):
//   * The pump thread is the ONLY thread that converts/downmixes/frames/drifts.
//   * Capture/render backends only move bytes between the device and a RingBuffer.
//   * The pump waits on capBackend->dataReadyEvent() (the capture stream signals
//     it after each ring write); on stop we SetEvent that same handle to wake it.
//   * Status fields are plain atomics (relaxed) so poll() never blocks the audio
//     path and never takes a lock.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MonitorEngine.h"
#include "ComUtil.h"
#include "DelayFifo.h"
#include "DeviceEnumerator.h"
#include "RingBuffer.h"
#include "SampleConvert.h"
#include "ScopeBuffer.h"
#include "WasapiStream.h"
#include <windows.h>
#include <algorithm>
#include <exception>
#include "Log.h"
#include "AudioFormatStr.h"

namespace wa {

namespace {
constexpr size_t kRingBytes      = 1u << 20; // 1 MiB per ring (>> 3x device period)
constexpr size_t kMaxChunkFrames = 4096;     // pump scratch / read granularity
constexpr DWORD  kPumpWaitMs     = 20;       // wake-up timeout fallback

// Read only whole frames: returns the number of frames actually read (a multiple
// of frameBytes). Never returns a partial frame, even if the ring holds one.
size_t readWholeFrames(RingBuffer& ring, uint8_t* dst, uint32_t frameBytes, size_t maxFrames) {
    const size_t availFrames = ring.availableRead() / frameBytes; // floor -> whole frames
    size_t frames = (availFrames < maxFrames) ? availFrames : maxFrames;
    if (frames == 0) return 0;
    const size_t got = ring.read(dst, frames * frameBytes);
    return got / frameBytes; // guard against a short read leaving a partial frame
}

Result sameRenderEndpoint(const DeviceId& a, const DeviceId& b, bool& same) {
    same = false;
    if (a.empty() && b.empty()) {
        same = true;
        return Result::Ok();
    }
    if (!a.empty() && !b.empty()) {
        same = (a == b);
        return Result::Ok();
    }

    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "MonitorEngine: CoInitializeEx");
    DeviceInfo def{};
    DeviceEnumerator de;
    Result r = de.defaultDevice(DataFlow::Render, def);
    if (!r) return r;
    same = a.empty() ? (def.id == b) : (a == def.id);
    return Result::Ok();
}
} // namespace

MonitorEngine::MonitorEngine(BackendFactory factory, SilentRenderFactory silentFactory)
    : factory_(std::move(factory)), silentFactory_(std::move(silentFactory)) {}
MonitorEngine::~MonitorEngine() { teardown(); }

std::unique_ptr<IAudioBackend> MonitorEngine::makeBackend(DataFlow flow, BackendKind kind,
                                                          const CaptureSource* source,
                                                          const AudioFormat* requested) {
    if (factory_) return factory_(flow, source, requested);
    const WasapiMode mode = (kind == BackendKind::WasapiExclusive) ? WasapiMode::Exclusive
                                                                   : WasapiMode::Shared;
    if (flow == DataFlow::Capture) {
        if (source && source->kind == CaptureSourceKind::SystemLoopback)
            return std::make_unique<WasapiSystemLoopbackCaptureStream>(mode, requested);
        return std::make_unique<WasapiCaptureStream>(mode, requested);
    }
    return std::make_unique<WasapiRenderStream>(mode, requested);
}

std::unique_ptr<IAudioBackend> MonitorEngine::makeSilentRenderBackend(
    const AudioFormat* requested) {
    if (silentFactory_) return silentFactory_(requested);
    return std::make_unique<WasapiSilentRenderStream>(WasapiMode::Shared, requested);
}

Result MonitorEngine::startSilentRenderIfNeeded() {
    silentRenderState_.store(StreamState::Idle, std::memory_order_relaxed);
    if (capSource_.kind != CaptureSourceKind::SystemLoopback || !loopbackOptions_.silentRender)
        return Result::Ok();

    WA_LOG(wa::log::Level::Info, "MonitorEngine", "silentRender.start",
           "dev=" + wa::narrowAscii(capSource_.deviceId), "requested");

    silentRenderBackend_ = makeSilentRenderBackend(&capFmt_);
    if (!silentRenderBackend_) {
        silentRenderState_.store(StreamState::Error, std::memory_order_relaxed);
        return Result::Fail(-1, "MonitorEngine: silent render factory null");
    }

    Result r = silentRenderBackend_->open(capSource_.deviceId, AudioFormat{}, nullptr, {});
    if (!r) {
        WA_LOG(wa::log::Level::Err, "MonitorEngine", "silentRender.open", "", r.message);
        silentRenderBackend_.reset();
        silentRenderState_.store(StreamState::Error, std::memory_order_relaxed);
        return r;
    }

    r = silentRenderBackend_->start();
    if (!r) {
        WA_LOG(wa::log::Level::Err, "MonitorEngine", "silentRender.start", "", r.message);
        silentRenderBackend_->stop();
        silentRenderBackend_.reset();
        silentRenderState_.store(StreamState::Error, std::memory_order_relaxed);
        return r;
    }

    silentRenderState_.store(StreamState::Running, std::memory_order_relaxed);
    WA_LOG(wa::log::Level::Info, "MonitorEngine", "silentRender.started", "", "ok");
    return Result::Ok();
}

void MonitorEngine::stopSilentRender() {
    if (silentRenderBackend_) {
        WA_LOG(wa::log::Level::Info, "MonitorEngine", "silentRender.stop", "", "");
        silentRenderBackend_->stop();
        silentRenderBackend_.reset();
    }
    if (silentRenderState_.load(std::memory_order_relaxed) == StreamState::Running)
        silentRenderState_.store(StreamState::Idle, std::memory_order_relaxed);
}

Result MonitorEngine::rollback(StreamState finalState, MonitorError err, long code,
                               std::string msg) {
    teardown();
    capState_.store(StreamState::Idle, std::memory_order_relaxed);
    renderState_.store(StreamState::Idle, std::memory_order_relaxed);
    overall_.store(finalState, std::memory_order_relaxed);
    errorCode_.store(static_cast<uint32_t>(err), std::memory_order_relaxed);
    return Result::Fail(code, std::move(msg));
}

Result MonitorEngine::guardLoopbackFeedback() {
    if (capSource_.kind != CaptureSourceKind::SystemLoopback) return Result::Ok();
    bool same = false;
    if (Result r = sameRenderEndpoint(capSource_.deviceId, renderId_, same); !r) return r;
    if (same) {
        return Result::Fail(static_cast<long>(MonitorError::LoopbackFeedback),
                            "MonitorEngine: loopback source and render output are the same device");
    }
    return Result::Ok();
}

Result MonitorEngine::start(BackendKind kind, const CaptureSource& capSource,
                            const DeviceId& renderId, uint32_t delayMs, bool playbackEnabled,
                            const StreamParams& capParams, const StreamParams& renderParams,
                            const AudioFormat* capFormat,
                            const LoopbackOptions& loopbackOptions) {
    teardown();
    // Fresh status slate.
    overall_.store(StreamState::Idle, std::memory_order_relaxed);
    capState_.store(StreamState::Idle, std::memory_order_relaxed);
    renderState_.store(StreamState::Idle, std::memory_order_relaxed);
    silentRenderState_.store(StreamState::Idle, std::memory_order_relaxed);
    errorCode_.store(0, std::memory_order_relaxed);
    fifoFillMs_.store(0.f, std::memory_order_relaxed);
    driftFixes_.store(0, std::memory_order_relaxed);
    capXruns_.store(0, std::memory_order_relaxed);
    renderXruns_.store(0, std::memory_order_relaxed);
    renderDropped_.store(0, std::memory_order_relaxed);
    capLevel_.store(0.f, std::memory_order_relaxed);
    renderLevel_.store(0.f, std::memory_order_relaxed);
    prefilled_.store(false, std::memory_order_relaxed);
    sampleRate_.store(0, std::memory_order_relaxed);
    delayMsAtomic_.store(0, std::memory_order_relaxed);
    renderBufMs_.store(0, std::memory_order_relaxed);

    if (delayMs > 10000u)
        return rollback(StreamState::Error, MonitorError::InvalidDelay,
                        static_cast<long>(MonitorError::InvalidDelay),
                        "MonitorEngine: delayMs exceeds maximum (10000 ms)");

    kind_ = kind;
    capSource_ = capSource;
    renderId_ = renderId;
    delayMs_ = delayMs;
    loopbackOptions_ = loopbackOptions;
    if (playbackEnabled) {
        if (Result r = guardLoopbackFeedback(); !r)
            return rollback(StreamState::Error, MonitorError::LoopbackFeedback, r.code, r.message);
    }

    WA_LOG(wa::log::Level::Info, "MonitorEngine", "start",
           "cap=" + wa::narrowAscii(capSource.deviceId) +
           " source=" + std::string(capSource.kind == CaptureSourceKind::SystemLoopback ? "loopback" : "endpoint") +
           " ren=" + wa::narrowAscii(renderId) +
           " delay=" + std::to_string(delayMs) + "ms" +
           " playback=" + (playbackEnabled ? "1" : "0"), "");
    capParams_ = capParams;
    { std::lock_guard<std::mutex> lk(paramsMtx_); renderParams_ = renderParams; }
    hasCapFormat_ = (capFormat != nullptr);
    if (capFormat) capRequestedFormat_ = *capFormat;

    // --- Capture (always) ---
    captureRing_ = std::make_unique<RingBuffer>(kRingBytes);
    capBackend_  = makeBackend(DataFlow::Capture, kind, &capSource,
                               hasCapFormat_ ? &capRequestedFormat_ : nullptr);
    if (!capBackend_)
        return rollback(StreamState::Idle, MonitorError::Factory, -1, "MonitorEngine: capture factory null");
    {
        Result r = capBackend_->open(capSource.deviceId, AudioFormat{}, captureRing_.get(), capParams_);
        WA_LOG(wa::log::Level::Debug, "MonitorEngine", "capture.open",
               "dev=" + wa::narrowAscii(capSource.deviceId), r.ok ? "ok" : r.message);
        if (!r) return rollback(StreamState::Idle, MonitorError::CaptureOpen, r.code, r.message);
    }
    {
        Result r = capBackend_->start();
        WA_LOG(wa::log::Level::Debug, "MonitorEngine", "capture.start", "", r.ok ? "ok" : r.message);
        if (!r) return rollback(StreamState::Idle, MonitorError::CaptureStart, r.code, r.message);
    }
    capState_.store(StreamState::Running, std::memory_order_relaxed);
    capFmt_ = capBackend_->stats().actualFormat;
    WA_LOG(wa::log::Level::Info, "MonitorEngine", "capture.started",
           "dev=" + wa::narrowAscii(capSource.deviceId) + " fmt=" + wa::formatAudio(capFmt_), "ok");

    const uint32_t sr = capFmt_.sampleRate;
    capCh_         = capFmt_.channels ? capFmt_.channels : static_cast<uint16_t>(1);
    capFrameBytes_ = capFmt_.blockAlign();
    if (sr == 0 || capFrameBytes_ == 0)
        return rollback(StreamState::Error, MonitorError::CaptureStart, -1, "MonitorEngine: invalid capture format");

    if (Result silent = startSilentRenderIfNeeded(); !silent) {
        WA_LOG(wa::log::Level::Warn, "MonitorEngine", "silentRender.unavailable",
               "", silent.message);
    }

    // --- Session-lifetime buffers (allocated once; freed only in teardown) ---
    // Floor of 1M so snapshotLatest's n<=cap/2 contract covers the GUI's spectrogram-history
    // waveform snapshot (kSpecCols*kFftHop = 491520 samples ~= 10.2 s @ 48 kHz, ~4 MiB/scope);
    // sr*2 keeps headroom at high rates.
    const size_t scopeCap = std::max<size_t>(static_cast<size_t>(sr) * 2u, 1048576u);
    captureScope_ = std::make_unique<ScopeBuffer>(scopeCap);
    renderScope_  = std::make_unique<ScopeBuffer>(scopeCap);  // GUI reads every frame -> MUST stay alive
    maxChunkFrames_ = kMaxChunkFrames;
    capScratch_.assign(maxChunkFrames_ * capFrameBytes_, 0);
    capFloat_.assign(maxChunkFrames_ * capCh_, 0.f);
    capMono_.assign(maxChunkFrames_, 0.f);

    sampleRate_.store(sr, std::memory_order_relaxed);
    delayMsAtomic_.store(delayMs, std::memory_order_relaxed);
    capDataReadyEvent_ = capBackend_->dataReadyEvent();

    // --- Optional render at start (synchronous engage -> full rollback on failure = CLI parity) ---
    wantPlayback_.store(playbackEnabled, std::memory_order_relaxed);
    if (playbackEnabled) {
        if (Result r = engageRender(); !r) {
            long code = r.code;
            std::string msg = r.message;
            WA_LOG(wa::log::Level::Err, "MonitorEngine", "start.engageRender", "playback=1", msg);
            MonitorError err = (code == static_cast<long>(MonitorError::RateMismatch))
                                   ? MonitorError::RateMismatch
                               : (code == static_cast<long>(MonitorError::LoopbackFeedback))
                                   ? MonitorError::LoopbackFeedback
                                   : MonitorError::RenderStart;
            return rollback(StreamState::Error, err, code, std::move(msg));   // stops capture too
        }
    }

    // Capture is up -> engine Running (independent of render prefill).
    overall_.store(StreamState::Running, std::memory_order_relaxed);
    WA_LOG(wa::log::Level::Info, "MonitorEngine", "engine.running",
           "cap=" + wa::narrowAscii(capSource.deviceId) +
           " ren=" + wa::narrowAscii(renderId) +
           " sr=" + std::to_string(capFmt_.sampleRate), "ok");

    running_.store(true, std::memory_order_release);
    try {
        pump_ = std::thread(&MonitorEngine::pumpLoop, this);
    } catch (const std::exception& e) {
        running_.store(false, std::memory_order_relaxed);
        return rollback(StreamState::Error, MonitorError::PumpLaunch, -1,
                        std::string("MonitorEngine: pump launch failed: ") + e.what());
    }
    WA_LOG(wa::log::Level::Info, "MonitorEngine", "start", "pump ready", "ok");
    return Result::Ok();
}

Result MonitorEngine::engageRender() {
    if (Result r = guardLoopbackFeedback(); !r) return r;
    StreamParams rp;
    { std::lock_guard<std::mutex> lk(paramsMtx_); rp = renderParams_; }
    const uint32_t sr = capFmt_.sampleRate;
    renderRing_    = std::make_unique<RingBuffer>(kRingBytes);            // per-engage
    renderBackend_ = makeBackend(DataFlow::Render, kind_, nullptr, &capFmt_);
    if (!renderBackend_) { renderRing_.reset(); return Result::Fail(-1, "MonitorEngine: render factory null"); }
    {
        Result r = renderBackend_->open(renderId_, AudioFormat{}, renderRing_.get(), rp);
        WA_LOG(wa::log::Level::Debug, "MonitorEngine", "render.open",
               "dev=" + wa::narrowAscii(renderId_), r.ok ? "ok" : r.message);
        if (!r) { renderBackend_.reset(); renderRing_.reset(); return Result::Fail(r.code, r.message); }
    }
    {
        Result r = renderBackend_->start();
        WA_LOG(wa::log::Level::Debug, "MonitorEngine", "render.start", "", r.ok ? "ok" : r.message);
        if (!r) { renderBackend_->stop(); renderBackend_.reset(); renderRing_.reset(); return Result::Fail(r.code, r.message); }
    }
    renderFmt_ = renderBackend_->stats().actualFormat;
    if (renderFmt_.sampleRate == 0 || renderFmt_.sampleRate != sr) {
        WA_LOG(wa::log::Level::Warn, "MonitorEngine", "engageRender",
               "cap_sr=" + std::to_string(sr) + " ren_sr=" + std::to_string(renderFmt_.sampleRate),
               "rate mismatch");
        renderBackend_->stop(); renderBackend_.reset(); renderRing_.reset();
        return Result::Fail(static_cast<long>(MonitorError::RateMismatch),
                            "MonitorEngine: capture/render sample-rate mismatch");
    }
    renderCh_         = renderFmt_.channels ? renderFmt_.channels : static_cast<uint16_t>(1);
    renderFrameBytes_ = renderFmt_.blockAlign();
    if (renderFrameBytes_ == 0) {
        renderBackend_->stop(); renderBackend_.reset(); renderRing_.reset();
        return Result::Fail(-1, "MonitorEngine: invalid render frame size");
    }
    const BackendStats rstats = renderBackend_->stats();
    size_t periodFrames = rstats.bufferFrames ? rstats.bufferFrames : (sr / 100u);
    if (periodFrames == 0) periodFrames = 1;
    const size_t delayFrames    = static_cast<size_t>(static_cast<uint64_t>(delayMs_) * sr / 1000u);
    prefillFrames_              = delayFrames + periodFrames;
    const size_t capacityFrames = prefillFrames_ + sr + periodFrames;
    size_t deadbandFrames       = periodFrames < 64 ? 64 : periodFrames;
    delayFifo_ = std::make_unique<DelayFifo>(capCh_, delayFrames, capacityFrames, deadbandFrames);  // per-engage

    popBuf_.assign(maxChunkFrames_ * capCh_, 0.f);
    renderAdapt_.assign(maxChunkFrames_ * renderCh_, 0.f);
    renderMono_.assign(maxChunkFrames_, 0.f);
    renderBytes_.assign(maxChunkFrames_ * renderFrameBytes_, 0);

    renderBufMs_.store(rstats.bufferFrames
                           ? static_cast<uint32_t>(static_cast<uint64_t>(rstats.bufferFrames) * 1000u / sr) : 0u,
                       std::memory_order_relaxed);
    renderDropped_.store(0, std::memory_order_relaxed);
    renderXruns_.store(0, std::memory_order_relaxed);
    renderLevel_.store(0.f, std::memory_order_relaxed);
    prefilled_.store(false, std::memory_order_relaxed);          // this engage's fill not done yet
    renderState_.store(StreamState::Running, std::memory_order_relaxed); // device up (fills, then pops)
    WA_LOG(wa::log::Level::Info, "MonitorEngine", "render.engaged",
           "dev=" + wa::narrowAscii(renderId_) +
           " fmt=" + wa::formatAudio(renderFmt_), "ok");
    return Result::Ok();
}

void MonitorEngine::disengageRender() {
    WA_LOG(wa::log::Level::Info, "MonitorEngine", "render.disengaged", "", "");
    if (renderBackend_) renderBackend_->stop();
    renderBackend_.reset();
    renderRing_.reset();
    delayFifo_.reset();
    renderState_.store(StreamState::Idle, std::memory_order_relaxed);
    renderLevel_.store(0.f, std::memory_order_relaxed);
    renderBufMs_.store(0, std::memory_order_relaxed);
    fifoFillMs_.store(0.f, std::memory_order_relaxed);
    driftFixes_.store(0, std::memory_order_relaxed);
    prefilled_.store(false, std::memory_order_relaxed);
    // NOTE: renderScope_ is session-lifetime -> NOT touched here (GUI reads it every frame).
}

void MonitorEngine::setPlaybackEnabled(bool enabled) {
    wantPlayback_.store(enabled, std::memory_order_release);   // pump converges to this next iteration
}

void MonitorEngine::setRenderParams(const StreamParams& p) {
    WA_LOG(wa::log::Level::Debug, "MonitorEngine", "setRenderParams", "", "");
    std::lock_guard<std::mutex> lk(paramsMtx_);
    renderParams_ = p;
}

void MonitorEngine::pumpLoop() {
    wa::log::setThreadName("pump");
    ComInitGuard com;   // pump owns COM (it now does render backend COM lifetime on toggle)
    const uint16_t capCh         = capCh_;
    const uint32_t capFrameBytes = capFrameBytes_;
    const uint32_t sr            = capFmt_.sampleRate;
    const size_t   maxFrames     = maxChunkFrames_;
    HANDLE         evt           = static_cast<HANDLE>(capDataReadyEvent_);

    while (running_.load(std::memory_order_acquire)) {
        if (evt) WaitForSingleObject(evt, kPumpWaitMs);
        else     Sleep(5);
        if (!running_.load(std::memory_order_acquire)) break;

        // --- Converge render lifecycle to wantPlayback_ (pump-thread only) ---
        const bool want   = wantPlayback_.load(std::memory_order_acquire);
        const bool active = (renderBackend_ != nullptr);
        if (want && !active) {
            if (Result r = engageRender(); !r) {
                WA_LOG(wa::log::Level::Err, "MonitorEngine", "render.engage", "", r.message);
                renderState_.store(StreamState::Error, std::memory_order_relaxed);
                errorCode_.store(static_cast<uint32_t>(
                    r.code == static_cast<long>(MonitorError::RateMismatch)
                        ? MonitorError::RateMismatch
                    : r.code == static_cast<long>(MonitorError::LoopbackFeedback)
                        ? MonitorError::LoopbackFeedback
                        : MonitorError::RenderStart),
                    std::memory_order_relaxed);
                wantPlayback_.store(false, std::memory_order_relaxed); // don't retry every wake
                // capture continues untouched
            }
        } else if (!want && active) {
            disengageRender();
        }
        const bool renderActive = (renderBackend_ != nullptr);
        const uint16_t renderCh         = renderCh_;
        const uint32_t renderFrameBytes = renderFrameBytes_;

        // --- Drain capture in whole frames (capture path ALWAYS runs) ---
        for (;;) {
            if (!running_.load(std::memory_order_acquire)) break;
            const size_t frames = readWholeFrames(*captureRing_, capScratch_.data(), capFrameBytes, maxFrames);
            if (frames == 0) break;

            pcmToFloat(capScratch_.data(), frames, capFmt_, capFloat_.data());
            downmixMono(capFloat_.data(), frames, capCh, capMono_.data());
            captureScope_->push(capMono_.data(), frames);
            capLevel_.store(peakLevel(capMono_.data(), frames), std::memory_order_relaxed);

            if (!renderActive) continue;   // capture-only: do not touch FIFO / render

            delayFifo_->pushFrames(capFloat_.data(), frames);
            if (!prefilled_.load(std::memory_order_relaxed)) {
                if (delayFifo_->fillFrames() >= prefillFrames_)
                    prefilled_.store(true, std::memory_order_relaxed);
            } else {
                const size_t popped = delayFifo_->popFrames(popBuf_.data(), frames);
                if (popped > 0) {
                    downmixMono(popBuf_.data(), popped, capCh, renderMono_.data());
                    renderScope_->push(renderMono_.data(), popped);
                    renderLevel_.store(peakLevel(renderMono_.data(), popped), std::memory_order_relaxed);
                    adaptChannels(popBuf_.data(), capCh, renderAdapt_.data(), renderCh, popped);
                    floatToPcm(renderAdapt_.data(), popped, renderFmt_, renderBytes_.data());
                    const size_t wantBytes = static_cast<size_t>(popped) * renderFrameBytes;
                    const size_t freeBytes = renderRing_->availableWrite();
                    const size_t safeBytes = (std::min(wantBytes, freeBytes) / renderFrameBytes) * renderFrameBytes;
                    renderRing_->write(renderBytes_.data(), safeBytes);
                    renderDropped_.fetch_add((wantBytes - safeBytes) / renderFrameBytes, std::memory_order_relaxed);
                }
            }
        }

        // --- Publish status (guard render fields on renderActive) ---
        capXruns_.store(captureRing_->overruns(), std::memory_order_relaxed);
        if (renderActive) {
            fifoFillMs_.store(static_cast<float>(delayFifo_->lowpassFillFrames() * 1000.0 / (double)sr),
                              std::memory_order_relaxed);
            driftFixes_.store(delayFifo_->driftFixes(), std::memory_order_relaxed);
            if (prefilled_.load(std::memory_order_relaxed))
                renderXruns_.store(renderDropped_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    }
}

void MonitorEngine::teardown() {
    running_.store(false, std::memory_order_release);
    if (capBackend_) {
        // Wake the pump's wait so it observes running_ == false and exits.
        if (void* e = capBackend_->dataReadyEvent()) SetEvent(static_cast<HANDLE>(e));
    }
    if (pump_.joinable()) pump_.join();

    // Pump is gone -> safe to stop devices and free buffers (spec order: cap, then render).
    stopSilentRender();
    if (capBackend_)    capBackend_->stop();
    if (renderBackend_) renderBackend_->stop();
    capBackend_.reset();
    renderBackend_.reset();
    captureRing_.reset();
    renderRing_.reset();
    captureScope_.reset();
    renderScope_.reset();
    delayFifo_.reset();
    capDataReadyEvent_ = nullptr;
    prefilled_.store(false, std::memory_order_relaxed);
}

void MonitorEngine::stop() {
    WA_LOG(wa::log::Level::Info, "MonitorEngine", "stop", "", "");
    teardown();
    capState_.store(StreamState::Idle, std::memory_order_relaxed);
    renderState_.store(StreamState::Idle, std::memory_order_relaxed);
    overall_.store(StreamState::Idle, std::memory_order_relaxed);
}

MonitorStatus MonitorEngine::poll() {
    MonitorStatus s{};
    s.overall     = overall_.load(std::memory_order_relaxed);
    s.capState    = capState_.load(std::memory_order_relaxed);
    s.renderState = renderState_.load(std::memory_order_relaxed);
    s.silentRenderState = silentRenderState_.load(std::memory_order_relaxed);
    s.sampleRate  = sampleRate_.load(std::memory_order_relaxed);
    s.delayMs     = delayMsAtomic_.load(std::memory_order_relaxed);
    s.fifoFillMs  = fifoFillMs_.load(std::memory_order_relaxed);
    s.renderBufMs = renderBufMs_.load(std::memory_order_relaxed);
    s.driftFixes  = driftFixes_.load(std::memory_order_relaxed);
    s.capXruns    = capXruns_.load(std::memory_order_relaxed);
    s.renderXruns = renderXruns_.load(std::memory_order_relaxed);
    s.capLevel    = capLevel_.load(std::memory_order_relaxed);
    s.renderLevel = renderLevel_.load(std::memory_order_relaxed);
    s.errorCode   = errorCode_.load(std::memory_order_relaxed);
    return s;
}

bool MonitorEngine::snapshotCapture(size_t n, float* out, uint64_t& endIdxOut) {
    ScopeBuffer* s = captureScope_.get();
    return s ? s->snapshotLatest(n, out, endIdxOut) : false;
}

bool MonitorEngine::snapshotRender(size_t n, float* out, uint64_t& endIdxOut) {
    ScopeBuffer* s = renderScope_.get();
    return s ? s->snapshotLatest(n, out, endIdxOut) : false;
}

uint64_t MonitorEngine::capWritten() const {
    ScopeBuffer* s = captureScope_.get();
    return s ? s->totalWritten() : 0;
}

uint64_t MonitorEngine::renderWritten() const {
    ScopeBuffer* s = renderScope_.get();
    return s ? s->totalWritten() : 0;
}

} // namespace wa
