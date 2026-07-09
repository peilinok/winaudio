#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include "IAudioBackend.h"
#include "ComUtil.h"
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace wa {

class RingBuffer;

enum class WasapiMode { Shared, Exclusive };

// StreamParams -> WASAPI enum mapping (free functions, exposed for unit tests).
AUDIO_STREAM_CATEGORY mapCategory(AudioCategory c);
AUDCLNT_STREAMOPTIONS mapStreamOption(StreamOption o);

class WasapiStream : public IAudioBackend {
public:
    WasapiStream(WasapiMode mode, const AudioFormat* requested);
    ~WasapiStream() override;
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                const StreamParams& params) override;
    Result start() override;
    void   stop() override;
    void   close() override;
    BackendStats stats() const override;

protected:
    // Direction-specific hooks implemented by Capture/Render subclasses.
    virtual EDataFlow dataFlow() const = 0;
    virtual Result createService() = 0;   // GetService(IAudioCaptureClient/RenderClient)
    virtual void   preRoll() {}           // render: one silent buffer; capture: nothing
    virtual void   runLoop() = 0;         // drain/feed loop; runs while running_
    virtual void   resetService() = 0;    // Reset() the service ComPtr (called from close())
    virtual DWORD  extraSharedInitFlags() const { return 0; }

    bool isExclusive() const { return mode_ == WasapiMode::Exclusive; }

    // Scaffolding state visible to subclasses.
    RingBuffer* ring_ = nullptr;
    AudioFormat actualFormat_{};
    uint32_t    bufferFrames_ = 0;
    uint32_t    frameBytes_ = 0;
    std::atomic<bool> running_{false};
    void*       hEvent_ = nullptr;        // HANDLE
    ComPtr<IAudioClient> client_;

private:
    void   threadMain();
    void   signalReady(Result res);
    Result prepareClient(IMMDevice* dev); // mode-aware: negotiate format + Initialize; sets actualFormat_/frameBytes_
    Result applyClientProperties();  // Activate 之后、Initialize 之前;全默认时零调用
    Result applyDucking();           // Initialize 之后;OptOut 时设置会话 ducking 偏好

    WasapiMode   mode_;
    AudioFormat  requestedFormat_{};
    bool         hasRequested_ = false;
    std::thread  thread_;
    DeviceId     deviceId_;
    StreamParams params_{};

    std::mutex              readyMtx_;
    std::condition_variable readyCv_;
    bool                    ready_ = false;
    Result                  startResult_ = Result::Ok();
};

class WasapiCaptureStream : public WasapiStream {
public:
    WasapiCaptureStream(WasapiMode mode, const AudioFormat* requested);
    ~WasapiCaptureStream() override;
    Result start() override;   // creates pumpEvent_ alongside hEvent_
    void   close() override;   // closes pumpEvent_ after thread join
    void*  dataReadyEvent() const override { return pumpEvent_; }
protected:
    EDataFlow dataFlow() const override { return eCapture; }
    Result createService() override;
    void   runLoop() override;
    void   resetService() override { capture_.Reset(); }
private:
    ComPtr<IAudioCaptureClient> capture_;
    void* pumpEvent_ = nullptr; // auto-reset event; signaled after each ring write
};

class WasapiSystemLoopbackCaptureStream : public WasapiCaptureStream {
public:
    WasapiSystemLoopbackCaptureStream(WasapiMode mode, const AudioFormat* requested);
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                const StreamParams& params) override;
protected:
    EDataFlow dataFlow() const override { return eRender; }
    DWORD extraSharedInitFlags() const override { return AUDCLNT_STREAMFLAGS_LOOPBACK; }
};

class WasapiRenderStream : public WasapiStream {
public:
    WasapiRenderStream(WasapiMode mode, const AudioFormat* requested);
    ~WasapiRenderStream() override;
protected:
    EDataFlow dataFlow() const override { return eRender; }
    Result createService() override;
    void   preRoll() override;
    void   runLoop() override;
    void   resetService() override { render_.Reset(); }
private:
    ComPtr<IAudioRenderClient> render_;
};

} // namespace wa
