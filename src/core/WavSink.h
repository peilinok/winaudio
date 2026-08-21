#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "AudioFormat.h"
#include "Result.h"

namespace wa {

class RingBuffer;

enum class WavSinkState { Idle, Running, Error };

struct WavSinkStatus {
    WavSinkState state = WavSinkState::Idle;
    std::wstring path;
    std::wstring fileName;
    std::string  message;
};

// Side WAV writer: producer push() is non-blocking (preallocated ring only).
// File open/write/close run on a dedicated writer thread.
class WavSink {
public:
    WavSink();
    ~WavSink();

    WavSink(const WavSink&)            = delete;
    WavSink& operator=(const WavSink&) = delete;

    Result startExact(const std::wstring& path, const AudioFormat& fmt);
    Result start(const std::wstring& folder, const std::wstring& prefix, const AudioFormat& fmt);
    Result start(const std::wstring& folder, const char* prefix, const AudioFormat& fmt);
    size_t push(const void* data, size_t bytes);
    Result stop();
    WavSinkStatus poll() const;
    bool isRunning() const;

private:
    Result begin(const std::wstring& path, const AudioFormat& fmt);
    void   writerLoop();
    void   joinWriter();

    AudioFormat                 fmt_{};
    std::unique_ptr<RingBuffer> ring_;
    std::thread                 writer_;
    std::atomic<bool>           running_{false};
    std::atomic<bool>           overflow_{false};
    std::atomic<bool>           writeError_{false};
    std::atomic<WavSinkState>   state_{WavSinkState::Idle};
    std::wstring                path_;
    std::wstring                fileName_;
    std::string                 message_;
    mutable std::mutex          mtx_;
};

} // namespace wa
