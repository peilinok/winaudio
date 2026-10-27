#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "AudioFormat.h"
#include "Result.h"
#include "WavSinkOutput.h"

namespace wa {

class RingBuffer;

enum class WavSinkState { Idle, Running, Error };
enum class WavDumpPhase { Idle, Preparing, Accepting, Finalizing, Success, Failure, Cancelled };

struct WavSinkStatus {
    WavSinkState state = WavSinkState::Idle;
    WavDumpPhase phase = WavDumpPhase::Idle;
    uint64_t generation = 0;
    uint64_t source = 0;
    std::wstring path;
    std::wstring fileName;
    std::string  message;
};

// Side WAV writer: producer push() is non-blocking (preallocated ring only).
// Exactly one producer per sink; Active protects lifetime, not MPSC access.
// File open/write/close run on a dedicated writer thread.
// Control operations serialize internally; stop/requestStop may overlap push.
class WavSink {
public:
    explicit WavSink(WavSinkOutputFactory outputFactory = {});
    ~WavSink();

    WavSink(const WavSink&)            = delete;
    WavSink& operator=(const WavSink&) = delete;

    // sourceVersion, when supplied, must outlive this sink's current writer.
    Result startExact(const std::wstring& path, const AudioFormat& fmt, uint64_t source = 1,
                      const std::atomic<uint64_t>* sourceVersion = nullptr);
    Result start(const std::wstring& folder, const std::wstring& prefix, const AudioFormat& fmt,
                 uint64_t source = 1, const std::atomic<uint64_t>* sourceVersion = nullptr);
    Result start(const std::wstring& folder, const char* prefix, const AudioFormat& fmt,
                 uint64_t source = 1, const std::atomic<uint64_t>* sourceVersion = nullptr);
    size_t push(const void* data, size_t bytes, uint64_t source = 1);
    void requestStop(uint64_t source = 1);
    Result stop();
    WavSinkStatus poll() const;
    bool isRunning() const;
    bool hasFailed() const;

private:
    Result begin(const std::wstring& path, const AudioFormat& fmt, uint64_t source,
                 const std::atomic<uint64_t>* sourceVersion);
    void   writerLoop();
    void   joinWriter();
    Result stopLocked();

    AudioFormat                 fmt_{};
    std::unique_ptr<RingBuffer> ring_;
    std::thread                 writer_;
    alignas(8) std::atomic<uint64_t> gate_{2}; // Closed, generation zero; never reused.
    uint64_t                    generation_ = 0;
    alignas(8) std::atomic<uint64_t> source_{0};
    const std::atomic<uint64_t>* sourceVersion_ = nullptr;
    enum class Failure { None, Overflow, Output };
    std::atomic<Failure>        failure_{Failure::None};
    static_assert(std::atomic<Failure>::is_always_lock_free, "WAV failure query must be lock-free");
    std::wstring                path_;
    std::wstring                fileName_;
    std::string                 message_;
    mutable std::mutex          mtx_;
    WavSinkOutputFactory        outputFactory_;
    std::mutex                  controlMtx_;
    std::condition_variable     readyCv_;
    bool                        prepared_ = false;
    bool                        preparedReady_ = false;
    uint64_t                    completedGeneration_ = 0; // mtx_ protects the completion tuple.
    WavDumpPhase                completedPhase_ = WavDumpPhase::Idle;
};

} // namespace wa
