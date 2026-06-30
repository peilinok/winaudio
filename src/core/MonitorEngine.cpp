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
#include "DelayFifo.h"
#include "RingBuffer.h"
#include "SampleConvert.h"
#include "ScopeBuffer.h"
#include "WasapiStream.h"
#include <windows.h>
#include <algorithm>
#include <exception>

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
} // namespace

MonitorEngine::MonitorEngine(BackendFactory factory) : factory_(std::move(factory)) {}
MonitorEngine::~MonitorEngine() { teardown(); }

std::unique_ptr<IAudioBackend> MonitorEngine::makeBackend(DataFlow flow, BackendKind kind,
                                                          const AudioFormat* requested) {
    if (factory_) return factory_(flow);
    const WasapiMode mode = (kind == BackendKind::WasapiExclusive) ? WasapiMode::Exclusive
                                                                   : WasapiMode::Shared;
    if (flow == DataFlow::Capture)
        return std::make_unique<WasapiCaptureStream>(mode, requested);
    return std::make_unique<WasapiRenderStream>(mode, requested);
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

Result MonitorEngine::start(BackendKind kind, const DeviceId& capId, const DeviceId& renderId,
                            uint32_t delayMs) {
    teardown(); // clean any prior run

    // Fresh status slate.
    overall_.store(StreamState::Idle, std::memory_order_relaxed);
    capState_.store(StreamState::Idle, std::memory_order_relaxed);
    renderState_.store(StreamState::Idle, std::memory_order_relaxed);
    errorCode_.store(0, std::memory_order_relaxed);
    fifoFillMs_.store(0.f, std::memory_order_relaxed);
    driftFixes_.store(0, std::memory_order_relaxed);
    capXruns_.store(0, std::memory_order_relaxed);
    renderXruns_.store(0, std::memory_order_relaxed);
    capLevel_.store(0.f, std::memory_order_relaxed);
    renderLevel_.store(0.f, std::memory_order_relaxed);
    prefilled_.store(false, std::memory_order_relaxed);

    // --- Capture: build -> open -> start (its actualFormat is valid after start) ---
    captureRing_ = std::make_unique<RingBuffer>(kRingBytes);
    capBackend_  = makeBackend(DataFlow::Capture, kind, nullptr);
    if (!capBackend_)
        return rollback(StreamState::Idle, MonitorError::Factory, -1,
                        "MonitorEngine: capture factory returned null");
    if (Result r = capBackend_->open(capId, AudioFormat{}, captureRing_.get()); !r)
        return rollback(StreamState::Idle, MonitorError::CaptureOpen, r.code, r.message);
    if (Result r = capBackend_->start(); !r)
        return rollback(StreamState::Idle, MonitorError::CaptureStart, r.code, r.message);
    capState_.store(StreamState::Running, std::memory_order_relaxed);
    capFmt_ = capBackend_->stats().actualFormat;

    // --- Render: build -> open -> start. Requested format = capFmt so a real
    //     exclusive render is opened at the capture rate; shared render ignores it
    //     and negotiates its own mix format, which the rate check below validates. ---
    renderRing_    = std::make_unique<RingBuffer>(kRingBytes);
    renderBackend_ = makeBackend(DataFlow::Render, kind, &capFmt_);
    if (!renderBackend_)
        return rollback(StreamState::Idle, MonitorError::Factory, -1,
                        "MonitorEngine: render factory returned null");
    if (Result r = renderBackend_->open(renderId, AudioFormat{}, renderRing_.get()); !r)
        return rollback(StreamState::Idle, MonitorError::RenderOpen, r.code, r.message);
    if (Result r = renderBackend_->start(); !r)
        return rollback(StreamState::Idle, MonitorError::RenderStart, r.code, r.message);
    renderState_.store(StreamState::Running, std::memory_order_relaxed);
    renderFmt_ = renderBackend_->stats().actualFormat;

    // --- Rate check: no resampler, so capture and render must agree on rate. ---
    if (capFmt_.sampleRate == 0 || capFmt_.sampleRate != renderFmt_.sampleRate)
        return rollback(StreamState::Error, MonitorError::RateMismatch,
                        static_cast<long>(MonitorError::RateMismatch),
                        "MonitorEngine: capture/render sample-rate mismatch");

    capCh_            = capFmt_.channels    ? capFmt_.channels    : static_cast<uint16_t>(1);
    renderCh_         = renderFmt_.channels ? renderFmt_.channels : static_cast<uint16_t>(1);
    capFrameBytes_    = capFmt_.blockAlign();
    renderFrameBytes_ = renderFmt_.blockAlign();
    if (capFrameBytes_ == 0 || renderFrameBytes_ == 0)
        return rollback(StreamState::Error, MonitorError::RateMismatch, -1,
                        "MonitorEngine: invalid frame size");

    // --- Sizing: DelayFifo target = delayMs; prefill = delay + one render period. ---
    const uint32_t     sr     = capFmt_.sampleRate;
    const BackendStats rstats = renderBackend_->stats();
    size_t periodFrames = rstats.bufferFrames ? rstats.bufferFrames : (sr / 100u); // ~10 ms
    if (periodFrames == 0) periodFrames = 1;
    const size_t delayFrames    = static_cast<size_t>(static_cast<uint64_t>(delayMs) * sr / 1000u);
    prefillFrames_              = delayFrames + periodFrames;
    const size_t capacityFrames = prefillFrames_ + sr + periodFrames; // generous headroom
    size_t deadbandFrames       = periodFrames;                       // > 1 period of ring sawtooth
    if (deadbandFrames < 64) deadbandFrames = 64;

    delayFifo_    = std::make_unique<DelayFifo>(capCh_, delayFrames, capacityFrames, deadbandFrames);
    const size_t scopeCap = std::max<size_t>(static_cast<size_t>(sr) * 2u, 8192u);
    captureScope_ = std::make_unique<ScopeBuffer>(scopeCap);
    renderScope_  = std::make_unique<ScopeBuffer>(scopeCap);

    // Preallocate all pump scratch up front.
    maxChunkFrames_ = kMaxChunkFrames;
    capScratch_.assign(maxChunkFrames_ * capFrameBytes_, 0);
    capFloat_.assign(maxChunkFrames_ * capCh_, 0.f);
    capMono_.assign(maxChunkFrames_, 0.f);
    popBuf_.assign(maxChunkFrames_ * capCh_, 0.f);
    renderAdapt_.assign(maxChunkFrames_ * renderCh_, 0.f);
    renderMono_.assign(maxChunkFrames_, 0.f);
    renderBytes_.assign(maxChunkFrames_ * renderFrameBytes_, 0);

    // Status scalars.
    sampleRate_.store(sr, std::memory_order_relaxed);
    delayMsAtomic_.store(delayMs, std::memory_order_relaxed);
    renderBufMs_.store(rstats.bufferFrames
                           ? static_cast<uint32_t>(static_cast<uint64_t>(rstats.bufferFrames) * 1000u / sr)
                           : 0u,
                       std::memory_order_relaxed);

    capDataReadyEvent_ = capBackend_->dataReadyEvent();

    // --- Launch the pump. overall_ stays Idle until the pump finishes prefill. ---
    running_.store(true, std::memory_order_release);
    prefilled_.store(false, std::memory_order_relaxed);
    try {
        pump_ = std::thread(&MonitorEngine::pumpLoop, this);
    } catch (const std::exception& e) {
        running_.store(false, std::memory_order_relaxed);
        return rollback(StreamState::Error, MonitorError::PumpLaunch, -1,
                        std::string("MonitorEngine: pump launch failed: ") + e.what());
    }
    return Result::Ok();
}

void MonitorEngine::pumpLoop() {
    const uint16_t capCh            = capCh_;
    const uint16_t renderCh         = renderCh_;
    const uint32_t capFrameBytes    = capFrameBytes_;
    const uint32_t renderFrameBytes = renderFrameBytes_;
    const uint32_t sr               = capFmt_.sampleRate;
    const size_t   maxFrames        = maxChunkFrames_;
    HANDLE         evt              = static_cast<HANDLE>(capDataReadyEvent_);

    while (running_.load(std::memory_order_acquire)) {
        if (evt) WaitForSingleObject(evt, kPumpWaitMs);
        else     Sleep(5); // capture should always provide an event; defensive fallback
        if (!running_.load(std::memory_order_acquire)) break;

        // Drain the capture ring in whole frames.
        for (;;) {
            if (!running_.load(std::memory_order_acquire)) break;
            const size_t frames = readWholeFrames(*captureRing_, capScratch_.data(),
                                                  capFrameBytes, maxFrames);
            if (frames == 0) break;

            // Capture tap: float (keep channels), downmix to mono for the scope.
            pcmToFloat(capScratch_.data(), frames, capFmt_, capFloat_.data());
            downmixMono(capFloat_.data(), frames, capCh, capMono_.data());
            captureScope_->push(capMono_.data(), frames);
            capLevel_.store(peakLevel(capMono_.data(), frames), std::memory_order_relaxed);

            // Into the delay line (keeps capture channels).
            delayFifo_->pushFrames(capFloat_.data(), frames);

            if (!prefilled_.load(std::memory_order_relaxed)) {
                // Prefill phase: accumulate to delay + one render period, then go
                // Running. Do NOT pop here -- the just-filled buffer IS the delay.
                if (delayFifo_->fillFrames() >= prefillFrames_) {
                    prefilled_.store(true, std::memory_order_relaxed);
                    overall_.store(StreamState::Running, std::memory_order_relaxed);
                }
            } else {
                // Steady state: pop ~as many frames as we pushed; the drift
                // controller nudges occupancy back toward target.
                const size_t popped = delayFifo_->popFrames(popBuf_.data(), frames);
                if (popped > 0) {
                    downmixMono(popBuf_.data(), popped, capCh, renderMono_.data());
                    renderScope_->push(renderMono_.data(), popped);
                    renderLevel_.store(peakLevel(renderMono_.data(), popped),
                                       std::memory_order_relaxed);

                    adaptChannels(popBuf_.data(), capCh, renderAdapt_.data(), renderCh, popped);
                    floatToPcm(renderAdapt_.data(), popped, renderFmt_, renderBytes_.data());
                    // tryWrite: a short write means the render ring overran.
                    renderRing_->write(renderBytes_.data(), popped * renderFrameBytes);
                }
            }
        }

        // Publish status (cheap; once per wake-up).
        fifoFillMs_.store(static_cast<float>(delayFifo_->lowpassFillFrames() * 1000.0 /
                                             static_cast<double>(sr)),
                          std::memory_order_relaxed);
        driftFixes_.store(delayFifo_->driftFixes(), std::memory_order_relaxed);
        capXruns_.store(captureRing_->overruns(), std::memory_order_relaxed);
        if (prefilled_.load(std::memory_order_relaxed))
            renderXruns_.store(renderRing_->overruns(), std::memory_order_relaxed);
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
