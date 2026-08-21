#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include "ComUtil.h"
#include "IAudioBackend.h"
#include "StreamInit.h"
#include "WasapiStream.h"

namespace wa {

AudioFormat defaultApplicationLoopbackFormat();

// Application Loopback adapter around Shared Stream init: always supplies LOOPBACK
// extras, and on mix failure retries once with 44100/2/16 requested. Ordinary
// Tracks call Stream init directly and do not get this fallback.
Result streamInitApplicationLoopback(AudioClientInit& client, StreamInitRequest req,
                                     StreamInitOutcome& out);

// SetClientProperties when enabled, then Stream init (LOOPBACK + 44100 mix-fallback).
// Stream init does not own client properties.
Result applyApplicationLoopbackClientProperties(AudioClientInit& client,
                                                const StreamParams& params);
Result openApplicationLoopbackClient(AudioClientInit& client, StreamInitRequest req,
                                     StreamInitOutcome& out);

class ApplicationLoopbackCaptureStream : public IAudioBackend {
public:
    ApplicationLoopbackCaptureStream(WasapiMode mode, uint32_t processId,
                                     const AudioFormat* requested,
                                     ProcessLoopbackMode processLoopbackMode =
                                         ProcessLoopbackMode::IncludeTree);
    ~ApplicationLoopbackCaptureStream() override;

    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                const StreamParams& params) override;
    Result start() override;
    void   stop() override;
    void   close() override;
    BackendStats stats() const override;
    void* dataReadyEvent() const override { return pumpEvent_; }

private:
    void threadMain();
    void signalReady(Result res);
    Result activateClient();
    Result initializeClient();
    Result createService();
    void runLoop();

    WasapiMode mode_;
    uint32_t processId_ = 0;
    ProcessLoopbackMode processLoopbackMode_ = ProcessLoopbackMode::IncludeTree;
    bool hasRequested_ = false;
    AudioFormat requestedFormat_{};
    StreamParams params_{};
    RingBuffer* ring_ = nullptr;

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    AudioFormat actualFormat_{};
    uint32_t bufferFrames_ = 0;
    uint32_t frameBytes_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> idleSilenceFrames_{0};
    std::atomic<uint64_t> silentPacketFrames_{0};
    void* hEvent_ = nullptr;
    void* pumpEvent_ = nullptr;
    std::thread thread_;

    std::mutex readyMtx_;
    std::condition_variable readyCv_;
    bool ready_ = false;
    Result startResult_ = Result::Ok();
};

} // namespace wa
