#include "WasapiShared.h"
#include "RingBuffer.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>

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
    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&WasapiSharedCapture::threadMain, this);
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

    client_->GetBufferSize(&bufferFrames_);
    client_->SetEventHandle(static_cast<HANDLE>(hEvent_));
    if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(capture_.GetAddressOf())))) { running_ = false; return; }

    client_->Start();
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

} // namespace wa
