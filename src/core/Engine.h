#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "IAudioBackend.h"
#include "RingBuffer.h"

namespace wa {

enum class EngineState { Idle, Capturing, Playing, Error };
enum class BackendKind { WasapiShared, WasapiExclusive };

struct EngineStatus {
    EngineState state = EngineState::Idle;
    float       levelL = 0.f;
    float       levelR = 0.f;
    uint64_t    overruns = 0;
    uint64_t    underruns = 0;
    AudioFormat actualFormat{};
    uint32_t    elapsedMs = 0;
    std::string message;
};

class WavWriter;
class WavReader;

class Engine {
public:
    Engine();
    ~Engine();

    std::vector<DeviceInfo> enumerate(DataFlow flow);
    Result startCapture(BackendKind kind, const DeviceId& id, const std::wstring& wavPath,
                        const AudioFormat* requested = nullptr);
    Result startPlayback(BackendKind kind, const DeviceId& id, const std::wstring& wavPath,
                         const AudioFormat* requested = nullptr);
    Result probeFormat(BackendKind kind, DataFlow flow, const DeviceId& id,
                       const AudioFormat& fmt);
    void   stop();
    EngineStatus poll();

private:
    void captureLoop(std::wstring wavPath);
    void playbackLoop(std::wstring wavPath);

    std::unique_ptr<IAudioBackend> backend_;
    std::unique_ptr<RingBuffer>    ring_;
    std::thread                    pump_;
    std::atomic<bool>              running_{false};

    std::mutex   mtx_;
    EngineStatus status_;
    unsigned long long startTick_ = 0;
};

} // namespace wa
