#include "RemoteInjection.h"
#include "Log.h"
#include <algorithm>
#include <string>

namespace wa {
namespace {
Result failed(DWORD error, const char* operation) {
    return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(error)), operation);
}
}

RemoteThreadWait waitRemoteThread(HANDLE thread, uint32_t timeoutMs) {
    const DWORD outcome = WaitForSingleObject(thread, timeoutMs);
    const DWORD error = outcome == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    return {outcome, error};
}

RemoteInjection::~RemoteInjection() {
    if (poll()) releaseParameter();
    else {
        WA_LOG(wa::log::Level::Warn, "Attach", "remote cleanup pending at shutdown",
               "parameter retained until target exit", "not reclaimed");
    }
    if (thread_) CloseHandle(thread_);
    if (process_) CloseHandle(process_);
}

void RemoteInjection::releaseParameter() {
    if (!parameter_) return;
    if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
        const BOOL freed = VirtualFreeEx(process_, parameter_, 0, MEM_RELEASE);
        const DWORD error = freed ? ERROR_SUCCESS : GetLastError();
        WA_LOG(freed ? wa::log::Level::Debug : wa::log::Level::Warn, "Attach", "VirtualFreeEx",
               "remote parameter", wa::log::hrName(HRESULT_FROM_WIN32(error)));
    }
    parameter_ = nullptr;
}

Result RemoteInjection::begin(HANDLE process, LPTHREAD_START_ROUTINE entry,
                              const void* parameter, size_t bytes) {
    if (process_ || !entry || !parameter || bytes == 0)
        return failed(ERROR_INVALID_PARAMETER, "Attach: invalid remote operation");
    const BOOL duplicated = DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &process_,
                                            0, FALSE, DUPLICATE_SAME_ACCESS);
    DWORD error = duplicated ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "DuplicateHandle(process)", "",
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    if (!duplicated) return failed(error, "Attach: duplicate process handle");
    parameter_ = VirtualAllocEx(process_, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    error = parameter_ ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "VirtualAllocEx", "remote parameter bytes=" + std::to_string(bytes),
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    if (!parameter_) return failed(error, "Attach: VirtualAllocEx");
    SIZE_T written = 0;
    const BOOL copied = WriteProcessMemory(process_, parameter_, parameter, bytes, &written);
    error = copied ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "WriteProcessMemory", "remote parameter",
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    if (!copied) return failed(error, "Attach: WriteProcessMemory");
    if (written != bytes) return failed(ERROR_PARTIAL_COPY, "Attach: incomplete remote parameter");
    thread_ = CreateRemoteThread(process_, nullptr, 0, entry, parameter_, 0, nullptr);
    error = thread_ ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "CreateRemoteThread", "",
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    if (!thread_) return failed(error, "Attach: CreateRemoteThread");
    return Result::Ok();
}

bool RemoteInjection::poll() {
    if (!thread_) return true;
    const DWORD target = WaitForSingleObject(process_, 0);
    if (target != WAIT_OBJECT_0 && WaitForSingleObject(thread_, 0) != WAIT_OBJECT_0) return false;
    if (target == WAIT_OBJECT_0) completion_ = failed(ERROR_PROCESS_ABORTED, "Attach: target exited");
    else if (!GetExitCodeThread(thread_, &exitCode_)) {
        const DWORD error = GetLastError();
        completion_ = failed(error, "Attach: GetExitCodeThread failed");
    }
    WA_LOG(completion_ ? wa::log::Level::Debug : wa::log::Level::Warn, "Attach", "remote thread completed",
           "exit=" + std::to_string(exitCode_), completion_ ? "S_OK" : completion_.message);
    CloseHandle(thread_);
    thread_ = nullptr;
    releaseParameter();
    return true;
}

Result RemoteInjection::wait(uint32_t timeoutMs, const std::atomic_bool* cancelled) {
    if (!thread_) return failed(ERROR_INVALID_HANDLE, "Attach: no remote thread");
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (cancelled && cancelled->load()) {
            return failed(ERROR_CANCELLED, "Attach: cancelled; remote cleanup pending");
        }
        if (poll()) return completion_;
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        const auto observed = waitRemoteThread(thread_, (std::min)(remaining, DWORD{25}));
        if (observed.outcome == WAIT_OBJECT_0) {
            poll();
            return completion_;
        }
        if (observed.outcome == WAIT_FAILED) {
            const DWORD error = observed.error;
            WA_LOG(wa::log::Level::Warn, "Attach", "WaitForSingleObject(remote)", "cleanup pending",
                   wa::log::hrName(HRESULT_FROM_WIN32(error)));
            return failed(error, "Attach: remote wait failed; cleanup pending");
        }
        if (GetTickCount64() >= deadline) {
            return failed(ERROR_TIMEOUT, "Attach: timed out; remote cleanup pending");
        }
    }
}

}  // namespace wa
