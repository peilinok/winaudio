#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Engine.h"
#include "Log.h"
#include "AudioFormatStr.h"
#include "WasapiStream.h"
#include "WavFile.h"
#include "DeviceEnumerator.h"
#include "ComUtil.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace wa {

namespace {
constexpr size_t kRingBytes = 1 << 20; // 1 MiB

// Compute peak level (0..1) for L/R from interleaved float32 or int16 PCM.
void computeLevels(const uint8_t* data, size_t bytes, const AudioFormat& fmt,
                   float& l, float& r) {
    l = r = 0.f;
    if (fmt.channels == 0) return;
    const uint16_t ch = fmt.channels;
    if (fmt.isFloat && fmt.bitsPerSample == 32) {
        const float* s = reinterpret_cast<const float*>(data);
        size_t n = bytes / 4;
        for (size_t i = 0; i + ch <= n; i += ch) {
            l = std::max(l, std::fabs(s[i]));
            r = std::max(r, std::fabs(s[i + (ch > 1 ? 1 : 0)]));
        }
    } else if (!fmt.isFloat && fmt.bitsPerSample == 16) {
        const int16_t* s = reinterpret_cast<const int16_t*>(data);
        size_t n = bytes / 2;
        for (size_t i = 0; i + ch <= n; i += ch) {
            l = std::max(l, std::fabs(s[i] / 32768.f));
            r = std::max(r, std::fabs(s[i + (ch > 1 ? 1 : 0)] / 32768.f));
        }
    }
}

std::unique_ptr<IAudioBackend> makeCaptureBackend(BackendKind kind, const CaptureSource& source,
                                                  const AudioFormat* requested) {
    const WasapiMode mode = (kind == BackendKind::WasapiExclusive) ? WasapiMode::Exclusive
                                                                   : WasapiMode::Shared;
    if (source.kind == CaptureSourceKind::SystemLoopback) {
        return std::make_unique<WasapiSystemLoopbackCaptureStream>(mode, requested);
    }
    return std::make_unique<WasapiCaptureStream>(mode, requested);
}

const char* captureSourceName(CaptureSourceKind kind) {
    return kind == CaptureSourceKind::SystemLoopback ? "system-loopback" : "endpoint";
}
} // namespace

Engine::Engine() = default;
Engine::~Engine() { stop(); }

std::vector<DeviceInfo> Engine::enumerate(DataFlow flow) {
    ComInitGuard com;
    DeviceEnumerator de;
    std::vector<DeviceInfo> out;
    de.enumerate(flow, out);
    return out;
}

Result Engine::probeFormat(BackendKind kind, DataFlow flow, const DeviceId& id,
                           const AudioFormat& fmt) {
    ComInitGuard com;
    WA_LOG(wa::log::Level::Info, "Engine", "probeFormat",
        "kind=" + std::string(kind == BackendKind::WasapiExclusive ? "exclusive" : "shared")
        + " flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render")
        + " id=" + (id.empty() ? std::string("(default)") : wa::narrowAscii(id))
        + " fmt=" + wa::formatAudio(fmt), "");
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "Engine", "CoCreateInstance(MMDeviceEnumerator)", "", wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "Engine", "CoCreateInstance(MMDeviceEnumerator)", "", wa::log::hrName(hr));
        return HrToResult(hr, "probeFormat: CoCreateInstance");
    }
    ComPtr<IMMDevice> dev;
    EDataFlow ef = (flow == DataFlow::Capture) ? eCapture : eRender;
    hr = id.empty() ? e->GetDefaultAudioEndpoint(ef, eConsole, dev.GetAddressOf())
                    : e->GetDevice(id.c_str(), dev.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "Engine",
        id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
        id.empty() ? "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render")
                   : "id=" + wa::narrowAscii(id),
        wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "Engine",
            id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice", "", wa::log::hrName(hr));
        return HrToResult(hr, "probeFormat: GetDevice");
    }
    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "Engine", "IMMDevice::Activate(IAudioClient)", "", wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "Engine", "IMMDevice::Activate(IAudioClient)", "", wa::log::hrName(hr));
        return HrToResult(hr, "probeFormat: Activate");
    }
    AUDCLNT_SHAREMODE sm = (kind == BackendKind::WasapiExclusive)
                               ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
    WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(fmt);
    WAVEFORMATEX* closest = nullptr;
    hr = client->IsFormatSupported(sm, reinterpret_cast<WAVEFORMATEX*>(&wfx),
                                   (sm == AUDCLNT_SHAREMODE_EXCLUSIVE) ? nullptr : &closest);
    WA_LOG(wa::log::Level::Debug, "Engine", "IAudioClient::IsFormatSupported",
        "mode=" + std::string(sm == AUDCLNT_SHAREMODE_EXCLUSIVE ? "exclusive" : "shared")
        + " fmt=" + wa::formatAudio(fmt), wa::log::hrName(hr));
    if (closest) CoTaskMemFree(closest);
    if (hr == S_OK) {
        WA_LOG(wa::log::Level::Info, "Engine", "probeFormat", "fmt=" + wa::formatAudio(fmt), "supported");
        return Result::Ok();
    }
    if (hr == S_FALSE) {
        // Shared mode can convert (AUTOCONVERTPCM); treat convertible as supported.
        // Exclusive mode requires exact match (S_OK only).
        if (kind != BackendKind::WasapiExclusive) {
            WA_LOG(wa::log::Level::Info, "Engine", "probeFormat", "fmt=" + wa::formatAudio(fmt), "supported(convertible)");
            return Result::Ok();
        }
        WA_LOG(wa::log::Level::Info, "Engine", "probeFormat", "fmt=" + wa::formatAudio(fmt), "not_exact_exclusive");
        return Result::Fail(1, "format not supported exactly (closest available)");
    }
    WA_LOG(wa::log::Level::Err, "Engine", "probeFormat", "fmt=" + wa::formatAudio(fmt), wa::log::hrName(hr));
    return HrToResult(hr, "probeFormat: not supported");
}

Result Engine::startCapture(BackendKind kind, const CaptureSource& source,
                            const std::wstring& wavPath, const AudioFormat* requested) {
    stop();
    WA_LOG(wa::log::Level::Info, "Engine", "startCapture",
        "source=" + std::string(captureSourceName(source.kind))
        + " kind=" + std::string(kind == BackendKind::WasapiExclusive ? "exclusive" : "shared")
        + " id=" + (source.deviceId.empty() ? std::string("(default)") : wa::narrowAscii(source.deviceId))
        + (requested ? " fmt=" + wa::formatAudio(*requested) : std::string("")), "");
    try {
        ring_ = std::make_unique<RingBuffer>(kRingBytes);
        backend_ = makeCaptureBackend(kind, source, requested);
        Result r = backend_->open(source.deviceId, AudioFormat{}, ring_.get(), StreamParams{});
        if (!r) {
            WA_LOG(wa::log::Level::Err, "Engine", "startCapture", "open", r.message);
            return r;
        }
        r = backend_->start();
        if (!r) {
            WA_LOG(wa::log::Level::Err, "Engine", "startCapture", "start", r.message);
            return r;
        }
        running_.store(true);
        startTick_ = GetTickCount64();
        { std::lock_guard<std::mutex> lk(mtx_); status_ = {}; status_.state = EngineState::Capturing; }
        pump_ = std::thread(&Engine::captureLoop, this, wavPath);
        WA_LOG(wa::log::Level::Info, "Engine", "startCapture", "", "ok");
        return Result::Ok();
    } catch (const std::exception& e) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        status_.state = EngineState::Error;
        status_.message = e.what();
        WA_LOG(wa::log::Level::Err, "Engine", "startCapture", "", std::string(e.what()));
        return Result::Fail(-1, e.what());
    }
}

Result Engine::startPlayback(BackendKind kind, const DeviceId& id, const std::wstring& wavPath,
                             const AudioFormat* requested) {
    stop();
    WA_LOG(wa::log::Level::Info, "Engine", "startPlayback",
        "kind=" + std::string(kind == BackendKind::WasapiExclusive ? "exclusive" : "shared")
        + " id=" + (id.empty() ? std::string("(default)") : wa::narrowAscii(id))
        + (requested ? " fmt=" + wa::formatAudio(*requested) : std::string("")), "");
    try {
        // Exclusive playback must run at the WAV file's own format (there is no
        // resampler); derive it from the file header instead of a UI/CLI value.
        AudioFormat wavFmt{};
        const AudioFormat* effReq = requested;
        if (kind == BackendKind::WasapiExclusive) {
            WavReader hdr;
            Result hr = hdr.open(wavPath);
            if (!hr) {
                WA_LOG(wa::log::Level::Err, "Engine", "startPlayback", "WavReader::open", hr.message);
                return hr;
            }
            wavFmt = hdr.format();
            hdr.close();
            effReq = &wavFmt;
        }
        ring_ = std::make_unique<RingBuffer>(kRingBytes);
        WasapiMode mode = (kind == BackendKind::WasapiExclusive) ? WasapiMode::Exclusive
                                                                 : WasapiMode::Shared;
        backend_ = std::make_unique<WasapiRenderStream>(mode, effReq);
        Result r = backend_->open(id, AudioFormat{}, ring_.get(), StreamParams{});
        if (!r) {
            WA_LOG(wa::log::Level::Err, "Engine", "startPlayback", "open", r.message);
            return r;
        }
        running_.store(true);
        startTick_ = GetTickCount64();
        { std::lock_guard<std::mutex> lk(mtx_); status_ = {}; status_.state = EngineState::Playing; }
        pump_ = std::thread(&Engine::playbackLoop, this, wavPath);
        WA_LOG(wa::log::Level::Info, "Engine", "startPlayback", "", "ok");
        return Result::Ok();
    } catch (const std::exception& e) {
        stop();
        std::lock_guard<std::mutex> lk(mtx_);
        status_.state = EngineState::Error;
        status_.message = e.what();
        WA_LOG(wa::log::Level::Err, "Engine", "startPlayback", "", std::string(e.what()));
        return Result::Fail(-1, e.what());
    }
}

void Engine::stop() {
    running_.store(false);
    if (pump_.joinable()) pump_.join();
    if (backend_) backend_->stop();
    backend_.reset();
    ring_.reset();
    std::lock_guard<std::mutex> lk(mtx_);
    if (status_.state != EngineState::Error) status_.state = EngineState::Idle;
}

void Engine::captureLoop(std::wstring wavPath) {
    wa::log::setThreadName("cap");
    // Backend already started; its actualFormat is known after start.
    AudioFormat fmt = backend_->stats().actualFormat;
    WavWriter writer;
    if (!writer.open(wavPath, fmt)) {
        std::lock_guard<std::mutex> lk(mtx_);
        status_.state = EngineState::Error;
        status_.message = "cannot open output wav";
        running_.store(false);
        return;
    }
    std::vector<uint8_t> buf(16384);
    while (running_.load()) {
        size_t got = ring_->read(buf.data(), buf.size());
        if (got == 0) { Sleep(5); }
        else {
            writer.write(buf.data(), got);
            float l, r; computeLevels(buf.data(), got, fmt, l, r);
            std::lock_guard<std::mutex> lk(mtx_);
            status_.levelL = l; status_.levelR = r;
            status_.actualFormat = fmt;
            status_.overruns = ring_->overruns();
            status_.underruns = ring_->underruns();
            status_.elapsedMs = static_cast<uint32_t>(GetTickCount64() - startTick_);
        }
    }
    writer.close();
}

void Engine::playbackLoop(std::wstring wavPath) {
    wa::log::setThreadName("play");
    WavReader reader;
    if (!reader.open(wavPath)) {
        std::lock_guard<std::mutex> lk(mtx_);
        status_.state = EngineState::Error;
        status_.message = "cannot open input wav";
        running_.store(false);
        return;
    }
    AudioFormat fmt = reader.format();
    {
        Result r = backend_->start();
        if (!r) {
            WA_LOG(wa::log::Level::Err, "Engine", "startPlayback", "backend::start", r.message);
            std::lock_guard<std::mutex> lk(mtx_);
            status_.state = EngineState::Error;
            status_.message = r.message;
            running_.store(false);
            return;
        }
    }
    std::vector<uint8_t> buf(16384);
    bool fileDone = false;
    while (running_.load()) {
        if (!fileDone) {
            // keep the ring topped up
            while (ring_->availableWrite() >= buf.size()) {
                size_t got = reader.read(buf.data(), buf.size());
                if (got == 0) { fileDone = true; break; }
                ring_->write(buf.data(), got);
                float l, r; computeLevels(buf.data(), got, fmt, l, r);
                std::lock_guard<std::mutex> lk(mtx_);
                status_.levelL = l; status_.levelR = r;
            }
        } else if (ring_->availableRead() == 0) {
            break; // drained
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            status_.actualFormat = backend_->stats().actualFormat;
            status_.overruns = ring_->overruns();
            status_.underruns = ring_->underruns();
            status_.elapsedMs = static_cast<uint32_t>(GetTickCount64() - startTick_);
        }
        Sleep(5);
    }
    running_.store(false);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (status_.state != EngineState::Error) status_.state = EngineState::Idle;
    }
}

EngineStatus Engine::poll() {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

} // namespace wa
