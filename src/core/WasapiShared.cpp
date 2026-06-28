#include "WasapiShared.h"
#include "RingBuffer.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <system_error>
#include <cstring>

namespace wa {

WasapiSharedCapture::~WasapiSharedCapture() { close(); }

Result WasapiSharedCapture::open(const DeviceId& id, const AudioFormat& /*fmt*/,
                                 RingBuffer* ring) {
    deviceId_ = id;
    ring_ = ring;
    return Result::Ok(); // real activation happens on the worker thread (its own COM apt)
}

Result WasapiSharedCapture::start() {
    if (running_.exchange(true)) return Result::Ok();
    // Close any handle left from a prior start()/stop() cycle before re-creating.
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent_) {
        running_.store(false);
        return Result::Fail(static_cast<long>(GetLastError()),
                            "WasapiSharedCapture::start: CreateEventW failed");
    }
    try {
        thread_ = std::thread(&WasapiSharedCapture::threadMain, this);
    } catch (const std::system_error& e) {
        CloseHandle(static_cast<HANDLE>(hEvent_));
        hEvent_ = nullptr;
        running_.store(false);
        return Result::Fail(static_cast<long>(e.code().value()),
                            "WasapiSharedCapture::start: failed to launch capture thread");
    }
    return Result::Ok();
}

void WasapiSharedCapture::stop() {
    running_.store(false);
    if (hEvent_) SetEvent(static_cast<HANDLE>(hEvent_));
    if (thread_.joinable()) thread_.join();
}

void WasapiSharedCapture::close() {
    stop();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    capture_.Reset();
    client_.Reset();
}

BackendStats WasapiSharedCapture::stats() const {
    BackendStats s{};
    s.actualFormat = actualFormat_;
    s.bufferFrames = bufferFrames_;
    if (ring_) { s.overruns = ring_->overruns(); s.underruns = ring_->underruns(); }
    return s;
}

void WasapiSharedCapture::threadMain() {
    ComInitGuard com; // this thread's own MTA apartment

    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf())))) { running_ = false; return; }

    ComPtr<IMMDevice> dev;
    HRESULT hr = deviceId_.empty()
        ? e->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)
        : e->GetDevice(deviceId_.c_str(), &dev);
    if (FAILED(hr)) { running_ = false; return; }

    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf())))) { running_ = false; return; }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client_->GetMixFormat(&mix)) || !mix) { running_ = false; return; }
    actualFormat_ = AudioFormat::fromWaveFormat(mix);
    const uint32_t frameBytes = actualFormat_.blockAlign();

    REFERENCE_TIME dur = 10'000'000 / 10; // 100 ms buffer
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) { running_ = false; return; }

    if (FAILED(client_->GetBufferSize(&bufferFrames_))) { running_ = false; return; }
    if (FAILED(client_->SetEventHandle(static_cast<HANDLE>(hEvent_)))) { running_ = false; return; }
    if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(capture_.GetAddressOf())))) { running_ = false; return; }

    if (FAILED(client_->Start())) { running_ = false; return; }
    while (running_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        UINT32 packet = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            const size_t bytes = static_cast<size_t>(frames) * frameBytes;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                static thread_local std::vector<uint8_t> zeros;
                zeros.assign(bytes, 0);
                ring_->write(zeros.data(), bytes);
            } else if (data) {
                ring_->write(data, bytes);
            }
            capture_->ReleaseBuffer(frames);
        }
    }
    client_->Stop();
}

// ---------------------------------------------------------------------------
// WasapiSharedRender
// ---------------------------------------------------------------------------

WasapiSharedRender::~WasapiSharedRender() { close(); }

Result WasapiSharedRender::open(const DeviceId& id, const AudioFormat& /*fmt*/,
                                RingBuffer* ring) {
    deviceId_ = id;
    ring_ = ring;
    return Result::Ok(); // real activation happens on the worker thread (its own COM apt)
}

Result WasapiSharedRender::start() {
    if (running_.exchange(true)) return Result::Ok();
    // Close any handle left from a prior start()/stop() cycle before re-creating.
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent_) {
        running_.store(false);
        return Result::Fail(static_cast<long>(GetLastError()),
                            "WasapiSharedRender::start: CreateEventW failed");
    }
    try {
        thread_ = std::thread(&WasapiSharedRender::threadMain, this);
    } catch (const std::system_error& e) {
        CloseHandle(static_cast<HANDLE>(hEvent_));
        hEvent_ = nullptr;
        running_.store(false);
        return Result::Fail(static_cast<long>(e.code().value()),
                            "WasapiSharedRender::start: failed to launch render thread");
    }
    return Result::Ok();
}

void WasapiSharedRender::stop() {
    running_.store(false);
    if (hEvent_) SetEvent(static_cast<HANDLE>(hEvent_));
    if (thread_.joinable()) thread_.join();
}

void WasapiSharedRender::close() {
    stop();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    render_.Reset();
    client_.Reset();
}

BackendStats WasapiSharedRender::stats() const {
    BackendStats s{};
    s.actualFormat = actualFormat_;
    s.bufferFrames = bufferFrames_;
    if (ring_) { s.overruns = ring_->overruns(); s.underruns = ring_->underruns(); }
    return s;
}

void WasapiSharedRender::threadMain() {
    ComInitGuard com; // this thread's own MTA apartment

    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf())))) { running_ = false; return; }

    ComPtr<IMMDevice> dev;
    HRESULT hr = deviceId_.empty()
        ? e->GetDefaultAudioEndpoint(eRender, eConsole, &dev)
        : e->GetDevice(deviceId_.c_str(), &dev);
    if (FAILED(hr)) { running_ = false; return; }

    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client_.GetAddressOf())))) { running_ = false; return; }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client_->GetMixFormat(&mix)) || !mix) { running_ = false; return; }
    actualFormat_ = AudioFormat::fromWaveFormat(mix);
    const uint32_t frameBytes = actualFormat_.blockAlign();

    REFERENCE_TIME dur = 10'000'000 / 10; // 100 ms buffer
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) { running_ = false; return; }

    if (FAILED(client_->GetBufferSize(&bufferFrames_))) { running_ = false; return; }
    if (FAILED(client_->SetEventHandle(static_cast<HANDLE>(hEvent_)))) { running_ = false; return; }
    if (FAILED(client_->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(render_.GetAddressOf())))) { running_ = false; return; }

    // Pre-roll one buffer of silence so the stream starts cleanly.
    BYTE* buf = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &buf)))
        render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);

    if (FAILED(client_->Start())) { running_ = false; return; }
    std::vector<uint8_t> scratch;
    while (running_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(hEvent_), 200);
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) break;
        UINT32 frames = bufferFrames_ - padding;
        if (frames == 0) continue;
        buf = nullptr;
        if (FAILED(render_->GetBuffer(frames, &buf))) break;
        const size_t want = static_cast<size_t>(frames) * frameBytes;
        scratch.resize(want);
        size_t got = ring_->read(scratch.data(), want);
        std::memcpy(buf, scratch.data(), got);
        if (got < want) std::memset(buf + got, 0, want - got); // underrun -> silence
        render_->ReleaseBuffer(frames, 0);
    }
    client_->Stop();
}

} // namespace wa
