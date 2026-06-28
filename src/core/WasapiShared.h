#pragma once
#include <atomic>
#include <thread>
#include "IAudioBackend.h"
#include "ComUtil.h"

struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioRenderClient;

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
};

} // namespace wa
