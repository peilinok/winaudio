#include "WasapiStream.h"
#include "RingBuffer.h"
#include "FormatSpec.h"
#include <system_error>
#include <cstring>

namespace wa {

WasapiStream::WasapiStream(WasapiMode mode, const AudioFormat* requested)
    : mode_(mode), hasRequested_(requested != nullptr) {
    if (requested) requestedFormat_ = *requested;
}

WasapiStream::~WasapiStream() { stop(); } // subclass dtor already ran close()

Result WasapiStream::open(const DeviceId& id, const AudioFormat& /*fmt*/, RingBuffer* ring) {
    deviceId_ = id;
    ring_ = ring;
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
    if (ring_) { s.overruns = ring_->overruns(); s.underruns = ring_->underruns(); }
    return s;
}

Result WasapiStream::prepareClient(IMMDevice* dev) {
    if (mode_ == WasapiMode::Shared) {
        WAVEFORMATEX* mix = nullptr;
        HRESULT hr = client_->GetMixFormat(&mix);
        if (FAILED(hr)) return HrToResult(hr, "WasapiStream: GetMixFormat");
        if (!mix) return Result::Fail(-1, "WasapiStream: GetMixFormat returned null");
        actualFormat_ = fromWaveFormat(mix);
        frameBytes_ = actualFormat_.blockAlign();
        REFERENCE_TIME dur = 10'000'000 / 10; // 100 ms buffer
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) return HrToResult(hr, "WasapiStream: Initialize(shared)");
        return Result::Ok();
    }
    // ---- Exclusive ----
    // Candidate formats: the explicitly requested one, else (capture only) a fallback list.
    std::vector<AudioFormat> candidates;
    if (hasRequested_) candidates.push_back(requestedFormat_);
    else if (dataFlow() == eCapture) candidates = defaultExclusiveCaptureCandidates();
    else return Result::Fail(-1, "WasapiStream: exclusive render requires an explicit format");

    int idx = selectSupportedFormat(candidates, [this](const AudioFormat& cand) {
        WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(cand);
        return client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                   reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr) == S_OK;
    });
    if (idx < 0)
        return Result::Fail(static_cast<long>(AUDCLNT_E_UNSUPPORTED_FORMAT),
                            "WasapiStream: no supported exclusive format");

    actualFormat_ = candidates[idx];
    frameBytes_ = actualFormat_.blockAlign();

    REFERENCE_TIME defPer = 0, minPer = 0;
    client_->GetDevicePeriod(&defPer, &minPer);
    REFERENCE_TIME dur = minPer;

    WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(actualFormat_);
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur,
                     reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        client_->GetBufferSize(&aligned);
        dur = alignedBufferDuration100ns(actualFormat_.sampleRate, aligned);
        // MSDN: the client must be rebuilt before re-Initializing with the aligned size.
        client_.Reset();
        HRESULT hr2 = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client_.GetAddressOf()));
        if (FAILED(hr2)) return HrToResult(hr2, "WasapiStream: exclusive realign Activate");
        WAVEFORMATEXTENSIBLE wfx2 = toWaveFormatExtensible(actualFormat_);
        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur,
                 reinterpret_cast<WAVEFORMATEX*>(&wfx2), nullptr);
    }
    if (FAILED(hr)) return HrToResult(hr, "WasapiStream: Initialize(exclusive)");
    return Result::Ok();
}

void WasapiStream::threadMain() {
    ComInitGuard com; // this thread's own MTA apartment

    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: CoCreateInstance")); return; }

    ComPtr<IMMDevice> dev;
    hr = deviceId_.empty()
        ? e->GetDefaultAudioEndpoint(dataFlow(), eConsole, &dev)
        : e->GetDevice(deviceId_.c_str(), &dev);
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: GetDevice")); return; }

    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: Activate")); return; }

    Result pr = prepareClient(dev.Get());
    if (!pr) { signalReady(pr); return; }

    hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: GetBufferSize")); return; }
    hr = client_->SetEventHandle(static_cast<HANDLE>(hEvent_));
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: SetEventHandle")); return; }

    Result cs = createService();
    if (!cs) { signalReady(cs); return; }

    preRoll();

    hr = client_->Start();
    if (FAILED(hr)) { signalReady(HrToResult(hr, "WasapiStream: Start")); return; }

    signalReady(Result::Ok()); // device ready; actualFormat_/bufferFrames_ valid

    runLoop();

    client_->Stop();
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
    if (FAILED(hr)) return HrToResult(hr, "WasapiCaptureStream: GetService");
    return Result::Ok();
}

void WasapiCaptureStream::runLoop() {
    while (running_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        UINT32 packet = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            const size_t bytes = static_cast<size_t>(frames) * frameBytes_;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                static thread_local std::vector<uint8_t> zeros;
                zeros.assign(bytes, 0);
                ring_->write(zeros.data(), bytes);
                if (pumpEvent_) SetEvent(static_cast<HANDLE>(pumpEvent_));
            } else if (data) {
                ring_->write(data, bytes);
                if (pumpEvent_) SetEvent(static_cast<HANDLE>(pumpEvent_));
            }
            capture_->ReleaseBuffer(frames);
        }
    }
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
    if (FAILED(hr)) return HrToResult(hr, "WasapiRenderStream: GetService");
    return Result::Ok();
}

void WasapiRenderStream::preRoll() {
    BYTE* buf = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &buf)))
        render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
}

void WasapiRenderStream::runLoop() {
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
            if (FAILED(client_->GetCurrentPadding(&padding))) break;
            frames = bufferFrames_ - padding;
            if (frames == 0) continue;
        }
        BYTE* buf = nullptr;
        if (FAILED(render_->GetBuffer(frames, &buf))) break;
        const size_t want = static_cast<size_t>(frames) * frameBytes_;
        scratch.resize(want);
        size_t got = ring_->read(scratch.data(), want);
        std::memcpy(buf, scratch.data(), got);
        if (got < want) std::memset(buf + got, 0, want - got); // underrun -> silence
        render_->ReleaseBuffer(frames, 0);
    }
}

} // namespace wa
