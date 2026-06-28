#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "IAudioBackend.h"
#include "ComUtil.h"
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace wa {

class RingBuffer;

class WasapiSharedCapture : public IAudioBackend {
public:
    ~WasapiSharedCapture() override;
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring) override;
    Result start() override;
    void   stop() override;
    void   close() override;
    BackendStats stats() const override;
private:
    void threadMain();

    RingBuffer* ring_ = nullptr;
    AudioFormat actualFormat_{};
    uint32_t    bufferFrames_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    DeviceId    deviceId_;
    void*       hEvent_ = nullptr;       // HANDLE
    ComPtr<IAudioClient>        client_;
    ComPtr<IAudioCaptureClient> capture_;

    std::mutex              readyMtx_;
    std::condition_variable readyCv_;
    bool                    ready_ = false;
    Result                  startResult_ = Result::Ok();
};

class WasapiSharedRender : public IAudioBackend {
public:
    ~WasapiSharedRender() override;
    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring) override;
    Result start() override;
    void   stop() override;
    void   close() override;
    BackendStats stats() const override;
private:
    void threadMain();

    RingBuffer* ring_ = nullptr;
    AudioFormat actualFormat_{};
    uint32_t    bufferFrames_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    DeviceId    deviceId_;
    void*       hEvent_ = nullptr;       // HANDLE
    ComPtr<IAudioClient>       client_;
    ComPtr<IAudioRenderClient> render_;

    std::mutex              readyMtx_;
    std::condition_variable readyCv_;
    bool                    ready_ = false;
    Result                  startResult_ = Result::Ok();
};

} // namespace wa
