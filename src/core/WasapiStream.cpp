#include "WasapiStream.h"
#include "RingBuffer.h"
#include "FormatSpec.h"
#include "StreamInit.h"
#include <audiopolicy.h>
#include <system_error>
#include <cstring>
#include "Log.h"
#include "AudioFormatStr.h"

namespace wa {

AUDIO_STREAM_CATEGORY mapCategory(AudioCategory c) {
    switch (c) {
    case AudioCategory::Communications: return AudioCategory_Communications;
    case AudioCategory::Media:          return AudioCategory_Media;
    case AudioCategory::Movie:          return AudioCategory_Movie;
    case AudioCategory::GameChat:       return AudioCategory_GameChat;
    case AudioCategory::Speech:         return AudioCategory_Speech;
    case AudioCategory::SoundEffects:   return AudioCategory_SoundEffects;
    case AudioCategory::GameMedia:      return AudioCategory_GameMedia;
    case AudioCategory::Other:
    default:                            return AudioCategory_Other;
    }
}

AUDCLNT_STREAMOPTIONS mapStreamOption(StreamOption o) {
    switch (o) {
    case StreamOption::Raw:                return AUDCLNT_STREAMOPTIONS_RAW;
    case StreamOption::MatchFormat:        return AUDCLNT_STREAMOPTIONS_MATCH_FORMAT;
    case StreamOption::Ambisonics:         return AUDCLNT_STREAMOPTIONS_AMBISONICS;
    case StreamOption::PostVolumeLoopback: return AUDCLNT_STREAMOPTIONS_POST_VOLUME_LOOPBACK;
    case StreamOption::None:
    default:                               return AUDCLNT_STREAMOPTIONS_NONE;
    }
}

uint32_t loopbackSilenceFramesForTimeout(uint32_t sampleRate, uint32_t timeoutMs) {
    return loopbackSilenceFramesForElapsed(sampleRate, timeoutMs, timeoutMs);
}

uint32_t loopbackSilenceFramesForElapsed(uint32_t sampleRate, uint64_t elapsedMs,
                                         uint32_t maxMs) {
    if (sampleRate == 0 || elapsedMs == 0 || maxMs == 0) return 0;
    const uint64_t boundedMs = elapsedMs < maxMs ? elapsedMs : maxMs;
    return static_cast<uint32_t>((static_cast<uint64_t>(sampleRate) * boundedMs) / 1000u);
}

uint32_t captureSilentPacketFrames(uint32_t frames, unsigned flags) {
    return (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? frames : 0u;
}

bool shouldWriteLoopbackIdleSilence(unsigned waitResult, long packetStatus,
                                    bool sawPacket, bool wroteFrames) {
    return (waitResult == WAIT_TIMEOUT || waitResult == WAIT_OBJECT_0) &&
           SUCCEEDED(static_cast<HRESULT>(packetStatus)) &&
           !sawPacket && !wroteFrames;
}

WasapiStream::WasapiStream(WasapiMode mode, const AudioFormat* requested)
    : mode_(mode), hasRequested_(requested != nullptr) {
    if (requested) requestedFormat_ = *requested;
}

WasapiStream::~WasapiStream() { stop(); } // subclass dtor already ran close()

Result WasapiStream::open(const DeviceId& id, const AudioFormat& /*fmt*/, RingBuffer* ring,
                          const StreamParams& params) {
    if (mode_ == WasapiMode::Exclusive &&
        (params.anyClientProps() || params.ducking != DuckingMode::Default)) {
        return Result::Fail(-1,
            "WasapiStream: advanced stream params (category/option/offload/ducking) require "
            "WASAPI-Shared; only bufferMs applies to exclusive mode");
    }
    deviceId_ = id;
    ring_ = ring;
    params_ = params;
    idleSilenceFrames_.store(0, std::memory_order_relaxed);
    silentPacketFrames_.store(0, std::memory_order_relaxed);
    return Result::Ok(); // real activation happens on the worker thread (its own COM apt)
}

void WasapiStream::signalReady(Result res) {
    { std::lock_guard<std::mutex> lk(readyMtx_); startResult_ = res; ready_ = true; }
    if (!res) running_.store(false);
    readyCv_.notify_one();
}

Result WasapiStream::start() {
    if (running_.exchange(true)) return Result::Ok();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent_) {
        running_.store(false);
        return Result::Fail(static_cast<long>(GetLastError()),
                            "WasapiStream::start: CreateEventW failed");
    }
    { std::lock_guard<std::mutex> lk(readyMtx_); ready_ = false; startResult_ = Result::Ok(); }
    try {
        thread_ = std::thread(&WasapiStream::threadMain, this);
    } catch (const std::system_error& e) {
        CloseHandle(static_cast<HANDLE>(hEvent_));
        hEvent_ = nullptr;
        running_.store(false);
        return Result::Fail(static_cast<long>(e.code().value()),
                            "WasapiStream::start: failed to launch thread");
    }
    Result r;
    {
        std::unique_lock<std::mutex> lk(readyMtx_);
        readyCv_.wait(lk, [this] { return ready_; });
        r = startResult_;
    }
    if (!r) stop();
    return r;
}

void WasapiStream::stop() {
    running_.store(false);
    if (hEvent_) SetEvent(static_cast<HANDLE>(hEvent_));
    if (thread_.joinable()) thread_.join();
}

void WasapiStream::close() {
    stop();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    resetService();
    client_.Reset();
}

BackendStats WasapiStream::stats() const {
    BackendStats s{};
    s.actualFormat = actualFormat_;
    s.bufferFrames = bufferFrames_;
    s.idleSilenceFrames = idleSilenceFrames_.load(std::memory_order_relaxed);
    s.silentPacketFrames = silentPacketFrames_.load(std::memory_order_relaxed);
    if (ring_) { s.overruns = ring_->overruns(); s.underruns = ring_->underruns(); }
    return s;
}

Result WasapiStream::applyClientProperties() {
    if (!params_.anyClientProps()) return Result::Ok();   // follow system: no calls at all
    ComPtr<IAudioClient2> client2;
    HRESULT hr = client_->QueryInterface(__uuidof(IAudioClient2),
                     reinterpret_cast<void**>(client2.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "QueryInterface(IAudioClient2)", "", wa::log::hrName(hr));
    if (FAILED(hr) || !client2.Get()) {
        WA_LOG(wa::log::Level::Err, "WasapiStream", "QueryInterface(IAudioClient2)", "", wa::log::hrName(FAILED(hr) ? hr : E_NOINTERFACE));
        return HrToResult(FAILED(hr) ? hr : E_NOINTERFACE,
                          "WasapiStream: IAudioClient2 unavailable; cannot apply advanced stream params");
    }
    const ClientProperties& cp = params_.clientProperties;
    const AUDIO_STREAM_CATEGORY cat = mapCategory(cp.category);
    if (cp.offload) {
        BOOL capable = FALSE;
        hr = client2->IsOffloadCapable(cat, &capable);
        WA_LOG(wa::log::Level::Debug, "WasapiStream", "IsOffloadCapable", "", wa::log::hrName(hr));
        if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "IsOffloadCapable", "", wa::log::hrName(hr)); return HrToResult(hr, "WasapiStream: IsOffloadCapable"); }
        if (!capable)
            return Result::Fail(-1, "WasapiStream: device/category does not support hardware offload");
    }
    AudioClientProperties p{};
    p.cbSize     = sizeof(p);
    p.bIsOffload = cp.offload ? TRUE : FALSE;
    p.eCategory  = cat;
    p.Options    = mapStreamOption(cp.option);
    hr = client2->SetClientProperties(&p);
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "SetClientProperties", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "SetClientProperties", "", wa::log::hrName(hr)); return HrToResult(hr, "WasapiStream: SetClientProperties"); }
    return Result::Ok();
}

Result WasapiStream::applyDucking() {
    if (params_.ducking != DuckingMode::OptOut) return Result::Ok();
    ComPtr<IAudioSessionControl> sc;
    HRESULT hr = client_->GetService(__uuidof(IAudioSessionControl),
                     reinterpret_cast<void**>(sc.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "GetService(IAudioSessionControl)", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "GetService(IAudioSessionControl)", "", wa::log::hrName(hr)); return HrToResult(hr, "WasapiStream: GetService(IAudioSessionControl)"); }
    ComPtr<IAudioSessionControl2> sc2;
    hr = sc->QueryInterface(__uuidof(IAudioSessionControl2),
                     reinterpret_cast<void**>(sc2.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "QueryInterface(IAudioSessionControl2)", "", wa::log::hrName(hr));
    if (FAILED(hr) || !sc2.Get()) {
        WA_LOG(wa::log::Level::Err, "WasapiStream", "QueryInterface(IAudioSessionControl2)", "", wa::log::hrName(FAILED(hr) ? hr : E_NOINTERFACE));
        return HrToResult(FAILED(hr) ? hr : E_NOINTERFACE, "WasapiStream: IAudioSessionControl2 unavailable");
    }
    hr = sc2->SetDuckingPreference(TRUE);
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "SetDuckingPreference", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "SetDuckingPreference", "", wa::log::hrName(hr)); return HrToResult(hr, "WasapiStream: SetDuckingPreference"); }
    return Result::Ok();
}

Result WasapiStream::prepareClient(IMMDevice* dev) {
    if (mode_ == WasapiMode::Shared) {
        AudioClientInitAdapter adapter(client_.Get());
        StreamInitRequest req;
        req.requested = hasRequested_ ? &requestedFormat_ : nullptr;
        req.params = params_;
        req.extraFlags = extraInitFlags_;
        StreamInitOutcome out;
        Result r = streamInitShared(adapter, req, out);
        if (r) {
            actualFormat_ = out.actualFormat;
            frameBytes_ = out.frameBytes;
        }
        return r;
    }
    // ---- Exclusive ----
    // Candidate formats: the explicitly requested one, else (capture only) a fallback list.
    std::vector<AudioFormat> candidates;
    if (hasRequested_) candidates.push_back(requestedFormat_);
    else if (dataFlow() == eCapture) candidates = defaultExclusiveCaptureCandidates();
    else return Result::Fail(-1, "WasapiStream: exclusive render requires an explicit format");

    int idx = selectSupportedFormat(candidates, [this](const AudioFormat& cand) {
        WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(cand);
        HRESULT hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                         reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
        WA_LOG(wa::log::Level::Debug, "WasapiStream", "IsFormatSupported(exclusive)", "fmt=" + wa::formatAudio(cand), wa::log::hrName(hr));
        return hr == S_OK;
    });
    if (idx < 0)
        return Result::Fail(static_cast<long>(AUDCLNT_E_UNSUPPORTED_FORMAT),
                            "WasapiStream: no supported exclusive format");

    actualFormat_ = candidates[idx];
    frameBytes_ = actualFormat_.blockAlign();

    REFERENCE_TIME defPer = 0, minPer = 0;
    HRESULT hrGP = client_->GetDevicePeriod(&defPer, &minPer);
    WA_LOG(wa::log::Level::Warn, "WasapiStream", "GetDevicePeriod", "", wa::log::hrName(hrGP));
    REFERENCE_TIME dur = params_.bufferMs
        ? static_cast<REFERENCE_TIME>(params_.bufferMs) * 10'000
        : minPer;

    WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(actualFormat_);
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur,
                     reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "Initialize(exclusive)", "fmt=" + wa::formatAudio(actualFormat_), wa::log::hrName(hr));
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        HRESULT hrGBS = client_->GetBufferSize(&aligned);
        WA_LOG(wa::log::Level::Warn, "WasapiStream", "GetBufferSize(realign)", "", wa::log::hrName(hrGBS));
        dur = alignedBufferDuration100ns(actualFormat_.sampleRate, aligned);
        // MSDN: the client must be rebuilt before re-Initializing with the aligned size.
        // Note: advanced client props (category/option/offload) are rejected in open() for
        // exclusive mode, so re-Activate here does not lose any SetClientProperties state.
        client_.Reset();
        HRESULT hr2 = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client_.GetAddressOf()));
        WA_LOG(wa::log::Level::Debug, "WasapiStream", "Activate(realign)", "", wa::log::hrName(hr2));
        if (FAILED(hr2)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "Activate(realign)", "", wa::log::hrName(hr2)); return HrToResult(hr2, "WasapiStream: exclusive realign Activate"); }
        WAVEFORMATEXTENSIBLE wfx2 = toWaveFormatExtensible(actualFormat_);
        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur,
                 reinterpret_cast<WAVEFORMATEX*>(&wfx2), nullptr);
        WA_LOG(wa::log::Level::Debug, "WasapiStream", "Initialize(exclusive,realign)", "fmt=" + wa::formatAudio(actualFormat_), wa::log::hrName(hr));
    }
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "Initialize(exclusive)", "fmt=" + wa::formatAudio(actualFormat_), wa::log::hrName(hr)); return HrToResult(hr, "WasapiStream: Initialize(exclusive)"); }
    return Result::Ok();
}

void WasapiStream::threadMain() {
    ComInitGuard com; // this thread's own MTA apartment
    WA_LOG(wa::log::Level::Info, "WasapiStream", "threadMain", "worker thread started", "");

    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "CoCreateInstance", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "CoCreateInstance", "", wa::log::hrName(hr)); signalReady(HrToResult(hr, "WasapiStream: CoCreateInstance")); return; }

    ComPtr<IMMDevice> dev;
    hr = deviceId_.empty()
        ? e->GetDefaultAudioEndpoint(dataFlow(), eConsole, &dev)
        : e->GetDevice(deviceId_.c_str(), &dev);
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "GetDevice", "id=" + wa::narrowAscii(deviceId_), wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "GetDevice", "id=" + wa::narrowAscii(deviceId_), wa::log::hrName(hr)); signalReady(HrToResult(hr, "WasapiStream: GetDevice")); return; }

    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "Activate", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "Activate", "", wa::log::hrName(hr)); signalReady(HrToResult(hr, "WasapiStream: Activate")); return; }

    if (Result ap = applyClientProperties(); !ap) { signalReady(ap); return; }

    Result pr = prepareClient(dev.Get());
    if (!pr) { signalReady(pr); return; }

    if (Result dk = applyDucking(); !dk) { signalReady(dk); return; }

    hr = client_->GetBufferSize(&bufferFrames_);
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "GetBufferSize", "frames=" + std::to_string(bufferFrames_), wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "GetBufferSize", "", wa::log::hrName(hr)); signalReady(HrToResult(hr, "WasapiStream: GetBufferSize")); return; }
    hr = client_->SetEventHandle(static_cast<HANDLE>(hEvent_));
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "SetEventHandle", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "SetEventHandle", "", wa::log::hrName(hr)); signalReady(HrToResult(hr, "WasapiStream: SetEventHandle")); return; }

    Result cs = createService();
    if (!cs) { signalReady(cs); return; }

    preRoll();

    hr = client_->Start();
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "Start", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "Start", "", wa::log::hrName(hr)); signalReady(HrToResult(hr, "WasapiStream: Start")); return; }

    WA_LOG(wa::log::Level::Info, "WasapiStream", "Start", "stream started", "");
    signalReady(Result::Ok()); // device ready; actualFormat_/bufferFrames_ valid

    runLoop();

    HRESULT hrStop = client_->Stop();
    WA_LOG(FAILED(hrStop) ? wa::log::Level::Err : wa::log::Level::Debug,
           "WasapiStream", "Stop", "", wa::log::hrName(hrStop));
}

// --------------------------------------------------------------------------
// WasapiCaptureStream
// --------------------------------------------------------------------------

WasapiCaptureStream::WasapiCaptureStream(WasapiMode mode, const AudioFormat* requested)
    : WasapiStream(mode, requested) {}

WasapiCaptureStream::~WasapiCaptureStream() { close(); }

Result WasapiCaptureStream::start() {
    // Already running: the base start() is idempotent (returns Ok without relaunching).
    // Do NOT rebuild pumpEvent_ here, or a pump already waiting on the prior handle breaks.
    if (running_.load(std::memory_order_acquire)) return WasapiStream::start();
    if (pumpEvent_) { CloseHandle(static_cast<HANDLE>(pumpEvent_)); pumpEvent_ = nullptr; }
    pumpEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!pumpEvent_) {
        return Result::Fail(static_cast<long>(GetLastError()),
                            "WasapiCaptureStream::start: CreateEventW failed");
    }
    Result r = WasapiStream::start();
    if (!r) { CloseHandle(static_cast<HANDLE>(pumpEvent_)); pumpEvent_ = nullptr; }
    return r;
}

void WasapiCaptureStream::close() {
    WasapiStream::close(); // stop() + join + close hEvent_ + resetService + client_.Reset()
    if (pumpEvent_) { CloseHandle(static_cast<HANDLE>(pumpEvent_)); pumpEvent_ = nullptr; }
}

Result WasapiCaptureStream::createService() {
    HRESULT hr = client_->GetService(__uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(capture_.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "GetService(IAudioCaptureClient)", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "GetService(IAudioCaptureClient)", "", wa::log::hrName(hr)); return HrToResult(hr, "WasapiCaptureStream: GetService"); }
    return Result::Ok();
}

void WasapiCaptureStream::runLoop() {
    wa::log::setThreadName("capW");
    WA_LOG(wa::log::Level::Info, "WasapiStream", "runLoop", "capture loop started", "");
    constexpr DWORD kCaptureWaitMs = 200;
    ULONGLONG lastWriteMs = GetTickCount64();
    while (running_.load()) {
        DWORD waitRc = WaitForSingleObject(static_cast<HANDLE>(hEvent_), kCaptureWaitMs);
        wa::log::emitTrace("WasapiStream", "WaitForSingleObject", 0, waitRc, 0);
        bool wroteFrames = false;
        bool sawPacket = false;
        UINT32 packet = 0;
        HRESULT hrNP = S_OK;
        while (hrNP = capture_->GetNextPacketSize(&packet),
               wa::log::emitTrace("WasapiStream", "GetNextPacketSize", packet, 0, (long)hrNP),
               SUCCEEDED(hrNP) && packet > 0) {
            sawPacket = true;
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            HRESULT hrGB = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            wa::log::emitTrace("WasapiStream", "GetBuffer", frames, (unsigned)flags, (long)hrGB);
            if (FAILED(hrGB))
                break;
            const size_t bytes = static_cast<size_t>(frames) * frameBytes_;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                static thread_local std::vector<uint8_t> zeros;
                zeros.assign(bytes, 0);
                ring_->write(zeros.data(), bytes);
                silentPacketFrames_.fetch_add(captureSilentPacketFrames(frames, flags),
                                              std::memory_order_relaxed);
                wroteFrames = true;
                lastWriteMs = GetTickCount64();
                if (pumpEvent_) SetEvent(static_cast<HANDLE>(pumpEvent_));
            } else if (data) {
                ring_->write(data, bytes);
                wroteFrames = true;
                lastWriteMs = GetTickCount64();
                if (pumpEvent_) SetEvent(static_cast<HANDLE>(pumpEvent_));
            }
            HRESULT hrRB = capture_->ReleaseBuffer(frames);
            wa::log::emitTrace("WasapiStream", "ReleaseBuffer", frames, 0, (long)hrRB);
        }
        if (shouldWriteLoopbackIdleSilence(waitRc, static_cast<long>(hrNP), sawPacket, wroteFrames)) {
            const ULONGLONG nowMs = GetTickCount64();
            const uint32_t frames = idleSilenceFrames(
                static_cast<uint32_t>(nowMs - lastWriteMs < kCaptureWaitMs
                                          ? nowMs - lastWriteMs
                                          : kCaptureWaitMs));
            if (frames > 0 && frameBytes_ > 0) {
                static thread_local std::vector<uint8_t> zeros;
                zeros.assign(static_cast<size_t>(frames) * frameBytes_, 0);
                ring_->write(zeros.data(), zeros.size());
                idleSilenceFrames_.fetch_add(frames, std::memory_order_relaxed);
                lastWriteMs = nowMs;
                if (pumpEvent_) SetEvent(static_cast<HANDLE>(pumpEvent_));
            }
        }
    }
}

// --------------------------------------------------------------------------
// WasapiSystemLoopbackCaptureStream
// --------------------------------------------------------------------------

WasapiSystemLoopbackCaptureStream::WasapiSystemLoopbackCaptureStream(
    WasapiMode mode, const AudioFormat* requested)
    : WasapiCaptureStream(mode, requested) {
    extraInitFlags_ = AUDCLNT_STREAMFLAGS_LOOPBACK;
}

Result WasapiSystemLoopbackCaptureStream::open(const DeviceId& id, const AudioFormat& fmt,
                                               RingBuffer* ring, const StreamParams& params) {
    if (isExclusive()) {
        return Result::Fail(-1, "WASAPI system loopback requires Shared mode");
    }
    return WasapiCaptureStream::open(id, fmt, ring, params);
}

// --------------------------------------------------------------------------
// WasapiRenderStream
// --------------------------------------------------------------------------

WasapiRenderStream::WasapiRenderStream(WasapiMode mode, const AudioFormat* requested)
    : WasapiStream(mode, requested) {}

WasapiRenderStream::~WasapiRenderStream() { close(); }

Result WasapiRenderStream::createService() {
    HRESULT hr = client_->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "GetService(IAudioRenderClient)", "", wa::log::hrName(hr));
    if (FAILED(hr)) { WA_LOG(wa::log::Level::Err, "WasapiStream", "GetService(IAudioRenderClient)", "", wa::log::hrName(hr)); return HrToResult(hr, "WasapiRenderStream: GetService"); }
    return Result::Ok();
}

void WasapiRenderStream::preRoll() {
    BYTE* buf = nullptr;
    HRESULT hrPre = render_->GetBuffer(bufferFrames_, &buf);
    WA_LOG(wa::log::Level::Debug, "WasapiStream", "GetBuffer(preRoll)",
           "frames=" + std::to_string(bufferFrames_), wa::log::hrName(hrPre));
    if (SUCCEEDED(hrPre)) {
        HRESULT hrRel = render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
        WA_LOG(wa::log::Level::Debug, "WasapiStream", "ReleaseBuffer(preRoll)", "",
               wa::log::hrName(hrRel));
    }
}

void WasapiRenderStream::runLoop() {
    wa::log::setThreadName("renW");
    WA_LOG(wa::log::Level::Info, "WasapiStream", "runLoop", "render loop started", "");
    const bool exclusive = isExclusive();
    std::vector<uint8_t> scratch;
    while (running_.load()) {
        DWORD waitRc = WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        UINT32 frames;
        if (exclusive) {
            // Exclusive event-driven: refill the whole buffer, but only on a real
            // buffer-ready event. On a wait timeout, retry rather than calling
            // GetBuffer prematurely (which would fail and kill the render thread).
            if (waitRc != WAIT_OBJECT_0) continue;
            frames = bufferFrames_;
        } else {
            UINT32 padding = 0;
            HRESULT hrPad = client_->GetCurrentPadding(&padding);
            wa::log::emitTrace("WasapiStream", "GetCurrentPadding", padding, 0, (long)hrPad);
            if (FAILED(hrPad)) break;
            frames = bufferFrames_ - padding;
            if (frames == 0) continue;
        }
        BYTE* buf = nullptr;
        HRESULT hrGB = render_->GetBuffer(frames, &buf);
        wa::log::emitTrace("WasapiStream", "GetBuffer", frames, 0, (long)hrGB);
        if (FAILED(hrGB)) break;
        const size_t want = static_cast<size_t>(frames) * frameBytes_;
        scratch.resize(want);
        size_t got = ring_->read(scratch.data(), want);
        std::memcpy(buf, scratch.data(), got);
        if (got < want) std::memset(buf + got, 0, want - got); // underrun -> silence
        HRESULT hrRB = render_->ReleaseBuffer(frames, 0);
        wa::log::emitTrace("WasapiStream", "ReleaseBuffer", frames, 0, (long)hrRB);
    }
}

WasapiSilentRenderStream::WasapiSilentRenderStream(WasapiMode mode,
                                                   const AudioFormat* requested)
    : WasapiStream(mode, requested) {}

WasapiSilentRenderStream::~WasapiSilentRenderStream() { close(); }

Result WasapiSilentRenderStream::open(const DeviceId& id, const AudioFormat& fmt,
                                      RingBuffer* ring, const StreamParams& params) {
    if (isExclusive()) {
        return Result::Fail(-1, "WasapiSilentRenderStream: silent render requires WASAPI-Shared");
    }
    return WasapiStream::open(id, fmt, ring, params);
}

Result WasapiSilentRenderStream::createService() {
    HRESULT hr = client_->GetService(__uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(render_.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "WasapiSilentRenderStream",
           "GetService(IAudioRenderClient)", "", wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "WasapiSilentRenderStream",
               "GetService(IAudioRenderClient)", "", wa::log::hrName(hr));
        return HrToResult(hr, "WasapiSilentRenderStream: GetService");
    }
    return Result::Ok();
}

void WasapiSilentRenderStream::preRoll() {
    BYTE* buf = nullptr;
    HRESULT hrPre = render_->GetBuffer(bufferFrames_, &buf);
    WA_LOG(wa::log::Level::Debug, "WasapiSilentRenderStream", "GetBuffer(preRoll)",
           "frames=" + std::to_string(bufferFrames_), wa::log::hrName(hrPre));
    if (SUCCEEDED(hrPre)) {
        HRESULT hrRel = render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
        WA_LOG(wa::log::Level::Debug, "WasapiSilentRenderStream",
               "ReleaseBuffer(preRoll)", "", wa::log::hrName(hrRel));
    }
}

void WasapiSilentRenderStream::runLoop() {
    wa::log::setThreadName("silR");
    WA_LOG(wa::log::Level::Info, "WasapiSilentRenderStream", "runLoop",
           "silent render loop started", "");
    while (running_.load()) {
        DWORD waitRc = WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        wa::log::emitTrace("WasapiSilentRenderStream", "WaitForSingleObject", 0, waitRc, 0);
        if (!running_.load()) break;

        UINT32 padding = 0;
        HRESULT hrPad = client_->GetCurrentPadding(&padding);
        wa::log::emitTrace("WasapiSilentRenderStream", "GetCurrentPadding", padding, 0,
                           static_cast<long>(hrPad));
        if (FAILED(hrPad)) break;

        UINT32 frames = bufferFrames_ - padding;
        if (frames == 0) continue;

        BYTE* buf = nullptr;
        HRESULT hrGB = render_->GetBuffer(frames, &buf);
        wa::log::emitTrace("WasapiSilentRenderStream", "GetBuffer", frames, 0,
                           static_cast<long>(hrGB));
        if (FAILED(hrGB)) break;

        HRESULT hrRB = render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
        wa::log::emitTrace("WasapiSilentRenderStream", "ReleaseBuffer", frames,
                           AUDCLNT_BUFFERFLAGS_SILENT, static_cast<long>(hrRB));
        if (FAILED(hrRB)) break;
    }
}

} // namespace wa
