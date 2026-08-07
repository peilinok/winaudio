#include "ApplicationLoopbackCapture.h"
#if defined(__has_include)
#if __has_include(<audioclientactivationparams.h>)
#define WA_HAS_AUDIOCLIENT_ACTIVATION_PARAMS 1
#include <audioclientactivationparams.h>
#endif
#endif
#ifndef WA_HAS_AUDIOCLIENT_ACTIVATION_PARAMS
#define WA_HAS_AUDIOCLIENT_ACTIVATION_PARAMS 0
#endif
#include <memory>
#include <propidl.h>
#include <wrl/implements.h>
#include "AudioFormat.h"
#include "AudioFormatStr.h"
#include "FormatSpec.h"
#include "Log.h"
#include "RingBuffer.h"

namespace wa {
namespace {

struct ActivationEvent {
    ActivationEvent() : handle(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~ActivationEvent() { if (handle) CloseHandle(handle); }
    HANDLE handle = nullptr;
};

class ActivationHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    explicit ActivationHandler(std::shared_ptr<ActivationEvent> done) : done_(std::move(done)) {}

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation) override {
        Microsoft::WRL::ComPtr<IUnknown> activated;
        HRESULT activateResult = E_UNEXPECTED;
        HRESULT hr = operation ? operation->GetActivateResult(&activateResult,
                                                              activated.GetAddressOf())
                               : E_POINTER;
        result_ = FAILED(hr) ? hr : activateResult;
        if (SUCCEEDED(result_)) activated_ = activated;
        if (done_ && done_->handle) SetEvent(done_->handle);
        return S_OK;
    }

    HRESULT copyClientTo(ComPtr<IAudioClient>& client) const {
        if (FAILED(result_)) return result_;
        if (!activated_) return E_POINTER;
        return activated_.As(&client);
    }

private:
    std::shared_ptr<ActivationEvent> done_;
    HRESULT result_ = E_UNEXPECTED;
    Microsoft::WRL::ComPtr<IUnknown> activated_;
};

Result activateProcessLoopbackClient(uint32_t processId, ProcessLoopbackMode processLoopbackMode,
                                     ComPtr<IAudioClient>& client) {
#if WA_HAS_AUDIOCLIENT_ACTIVATION_PARAMS && defined(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
    auto done = std::make_shared<ActivationEvent>();
    if (!done->handle) {
        return Result::Fail(static_cast<long>(GetLastError()),
                            "ApplicationLoopbackCaptureStream: CreateEventW failed");
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams{};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = processId;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
        (processLoopbackMode == ProcessLoopbackMode::ExcludeTree)
            ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
            : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    WA_LOG(wa::log::Level::Debug, "ApplicationLoopbackCaptureStream", "activate",
           "pid=" + std::to_string(processId) +
               " mode=" + processLoopbackModeName(processLoopbackMode),
           "");

    PROPVARIANT prop{};
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(activationParams);
    prop.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    auto handler = Microsoft::WRL::Make<ActivationHandler>(done);
    if (!handler) {
        return Result::Fail(E_OUTOFMEMORY, "ApplicationLoopbackCaptureStream: handler allocation failed");
    }

    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &prop, handler.Get(),
                                             op.GetAddressOf());
    if (SUCCEEDED(hr)) {
        DWORD wait = WaitForSingleObject(done->handle, 10000);
        if (wait == WAIT_OBJECT_0) {
            hr = handler->copyClientTo(client);
        } else if (wait == WAIT_TIMEOUT) {
            hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        } else {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }

    if (FAILED(hr)) {
        return HrToResult(hr,
            "ApplicationLoopbackCaptureStream: ActivateAudioInterfaceAsync "
            "(requires Windows 10 build 20348 or later)");
    }
    return Result::Ok();
#else
    (void)processId;
    (void)processLoopbackMode;
    (void)client;
    return Result::Fail(-1,
        "ApplicationLoopbackCaptureStream: Application Loopback requires Windows SDK 10.0.20348+");
#endif
}

} // namespace

ApplicationLoopbackCaptureStream::ApplicationLoopbackCaptureStream(
    WasapiMode mode, uint32_t processId, const AudioFormat* requested,
    ProcessLoopbackMode processLoopbackMode)
    : mode_(mode), processId_(processId), processLoopbackMode_(processLoopbackMode),
      hasRequested_(requested != nullptr) {
    if (requested) requestedFormat_ = *requested;
}

AudioFormat defaultApplicationLoopbackFormat() {
    return AudioFormat{44100, 2, 16, false};
}

Result initializeApplicationLoopbackClient(ComPtr<IAudioClient>& client, const AudioFormat& fmt,
                                           DWORD flags, REFERENCE_TIME dur,
                                           const char* where) {
    WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(fmt);
    HRESULT hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        dur, 0, reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    if (FAILED(hr)) return HrToResult(hr, where);
    return Result::Ok();
}

ApplicationLoopbackCaptureStream::~ApplicationLoopbackCaptureStream() { close(); }

Result ApplicationLoopbackCaptureStream::open(const DeviceId&, const AudioFormat&,
                                              RingBuffer* ring, const StreamParams& params) {
    if (mode_ == WasapiMode::Exclusive) {
        return Result::Fail(-1, "WASAPI application loopback requires Shared mode");
    }
    if (processId_ == 0) {
        return Result::Fail(-1, "ApplicationLoopbackCaptureStream: target PID is required");
    }
    if (!ring) {
        return Result::Fail(-1, "ApplicationLoopbackCaptureStream: capture ring is required");
    }
    ring_ = ring;
    params_ = params;
    idleSilenceFrames_.store(0, std::memory_order_relaxed);
    silentPacketFrames_.store(0, std::memory_order_relaxed);
    return Result::Ok();
}

Result ApplicationLoopbackCaptureStream::start() {
    if (running_.exchange(true)) return Result::Ok();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    if (pumpEvent_) { CloseHandle(static_cast<HANDLE>(pumpEvent_)); pumpEvent_ = nullptr; }

    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    pumpEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent_ || !pumpEvent_) {
        close();
        return Result::Fail(static_cast<long>(GetLastError()),
                            "ApplicationLoopbackCaptureStream::start: CreateEventW failed");
    }

    { std::lock_guard<std::mutex> lk(readyMtx_); ready_ = false; startResult_ = Result::Ok(); }
    try {
        thread_ = std::thread(&ApplicationLoopbackCaptureStream::threadMain, this);
    } catch (const std::system_error& e) {
        close();
        return Result::Fail(static_cast<long>(e.code().value()),
                            "ApplicationLoopbackCaptureStream::start: failed to launch thread");
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

void ApplicationLoopbackCaptureStream::stop() {
    running_.store(false);
    if (hEvent_) SetEvent(static_cast<HANDLE>(hEvent_));
    if (thread_.joinable()) thread_.join();
}

void ApplicationLoopbackCaptureStream::close() {
    stop();
    if (hEvent_) { CloseHandle(static_cast<HANDLE>(hEvent_)); hEvent_ = nullptr; }
    if (pumpEvent_) { CloseHandle(static_cast<HANDLE>(pumpEvent_)); pumpEvent_ = nullptr; }
    capture_.Reset();
    client_.Reset();
}

BackendStats ApplicationLoopbackCaptureStream::stats() const {
    BackendStats s{};
    s.actualFormat = actualFormat_;
    s.bufferFrames = bufferFrames_;
    s.idleSilenceFrames = idleSilenceFrames_.load(std::memory_order_relaxed);
    s.silentPacketFrames = silentPacketFrames_.load(std::memory_order_relaxed);
    if (ring_) { s.overruns = ring_->overruns(); s.underruns = ring_->underruns(); }
    return s;
}

void ApplicationLoopbackCaptureStream::signalReady(Result res) {
    { std::lock_guard<std::mutex> lk(readyMtx_); startResult_ = res; ready_ = true; }
    if (!res) running_.store(false);
    readyCv_.notify_one();
}

Result ApplicationLoopbackCaptureStream::activateClient() {
    return activateProcessLoopbackClient(processId_, processLoopbackMode_, client_);
}

Result ApplicationLoopbackCaptureStream::initializeClient() {
    REFERENCE_TIME dur = params_.bufferMs
        ? static_cast<REFERENCE_TIME>(params_.bufferMs) * 10000
        : 1000000; // 100 ms
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK;

    if (hasRequested_) {
        actualFormat_ = requestedFormat_;
        frameBytes_ = actualFormat_.blockAlign();
        return initializeApplicationLoopbackClient(client_, actualFormat_, flags, dur,
            "ApplicationLoopbackCaptureStream: Initialize(requested)");
    }

    WAVEFORMATEX* mix = nullptr;
    HRESULT hr = client_->GetMixFormat(&mix);
    if (SUCCEEDED(hr) && mix) {
        actualFormat_ = fromWaveFormat(mix);
        frameBytes_ = actualFormat_.blockAlign();
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (SUCCEEDED(hr)) return Result::Ok();
    }
    if (mix) CoTaskMemFree(mix);

    actualFormat_ = defaultApplicationLoopbackFormat();
    frameBytes_ = actualFormat_.blockAlign();
    return initializeApplicationLoopbackClient(client_, actualFormat_, flags, dur,
        "ApplicationLoopbackCaptureStream: Initialize(fallback)");
}

Result ApplicationLoopbackCaptureStream::createService() {
    HRESULT hr = client_->GetService(__uuidof(IAudioCaptureClient),
                                     reinterpret_cast<void**>(capture_.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "ApplicationLoopbackCaptureStream: GetService");
    return Result::Ok();
}

void ApplicationLoopbackCaptureStream::threadMain() {
    ComInitGuard com;
    WA_LOG(wa::log::Level::Info, "ApplicationLoopbackCaptureStream", "threadMain",
           "worker thread started", "");
    if (!com.ok()) {
        signalReady(HrToResult(com.hr, "ApplicationLoopbackCaptureStream: CoInitializeEx"));
        return;
    }

    if (Result r = activateClient(); !r) { signalReady(r); return; }
    if (Result r = initializeClient(); !r) { signalReady(r); return; }

    HRESULT hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr)) {
        signalReady(HrToResult(hr, "ApplicationLoopbackCaptureStream: GetBufferSize"));
        return;
    }
    hr = client_->SetEventHandle(static_cast<HANDLE>(hEvent_));
    if (FAILED(hr)) {
        signalReady(HrToResult(hr, "ApplicationLoopbackCaptureStream: SetEventHandle"));
        return;
    }
    if (Result r = createService(); !r) { signalReady(r); return; }

    hr = client_->Start();
    if (FAILED(hr)) {
        signalReady(HrToResult(hr, "ApplicationLoopbackCaptureStream: Start"));
        return;
    }

    WA_LOG(wa::log::Level::Info, "ApplicationLoopbackCaptureStream", "Start",
           "pid=" + std::to_string(processId_) +
               " mode=" + processLoopbackModeName(processLoopbackMode_) +
               " fmt=" + wa::formatAudio(actualFormat_),
           "ok");
    signalReady(Result::Ok());
    runLoop();

    HRESULT hrStop = client_->Stop();
    WA_LOG(FAILED(hrStop) ? wa::log::Level::Err : wa::log::Level::Debug,
           "ApplicationLoopbackCaptureStream", "Stop", "", wa::log::hrName(hrStop));
}

void ApplicationLoopbackCaptureStream::runLoop() {
    wa::log::setThreadName("appL");
    constexpr DWORD kCaptureWaitMs = 200;
    ULONGLONG lastWriteMs = GetTickCount64();
    while (running_.load()) {
        DWORD waitRc = WaitForSingleObject(static_cast<HANDLE>(hEvent_), kCaptureWaitMs);
        bool wroteFrames = false;
        bool sawPacket = false;
        UINT32 packet = 0;
        HRESULT hrNP = S_OK;
        while (hrNP = capture_->GetNextPacketSize(&packet),
               SUCCEEDED(hrNP) && packet > 0) {
            sawPacket = true;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            HRESULT hrGB = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hrGB)) break;
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
            capture_->ReleaseBuffer(frames);
        }

        if (shouldWriteLoopbackIdleSilence(waitRc, static_cast<long>(hrNP), sawPacket,
                                           wroteFrames)) {
            const ULONGLONG nowMs = GetTickCount64();
            const uint32_t elapsedMs = static_cast<uint32_t>(
                nowMs - lastWriteMs < kCaptureWaitMs ? nowMs - lastWriteMs : kCaptureWaitMs);
            const uint32_t frames = loopbackSilenceFramesForTimeout(actualFormat_.sampleRate,
                                                                    elapsedMs);
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

} // namespace wa
