#pragma once
#include "Result.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace wa {

struct RemoteThreadWait {
    uint32_t outcome = WAIT_FAILED;
    uint32_t error = ERROR_SUCCESS;
};

// Snapshot GetLastError immediately; WAIT_FAILED is not a timeout.
RemoteThreadWait waitRemoteThread(HANDLE thread, uint32_t timeoutMs);

// Owns the process reference, remote parameter allocation and remote thread.
// A timed-out caller still owns a live operation until poll() confirms completion.
class RemoteInjection {
public:
    ~RemoteInjection();
    RemoteInjection() = default;
    RemoteInjection(const RemoteInjection&) = delete;
    RemoteInjection& operator=(const RemoteInjection&) = delete;

    Result begin(HANDLE process, LPTHREAD_START_ROUTINE entry, const void* parameter, size_t bytes);
    Result wait(uint32_t timeoutMs, const std::atomic_bool* cancelled = nullptr);
    bool poll();
    bool pending() const { return thread_ != nullptr; }
    HANDLE process() const { return process_; }
    uint32_t exitCode() const { return exitCode_; }
    const void* parameter() const { return parameter_; }
    const Result& completionResult() const { return completion_; }

private:
    void releaseParameter();
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    void* parameter_ = nullptr;
    DWORD exitCode_ = 0;
    Result completion_;
};

}  // namespace wa
