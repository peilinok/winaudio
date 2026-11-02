#include "StartupLaunch.h"
#include "Log.h"
#include "AudioFormatStr.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <thread>
#include <exception>

namespace wa {
const char* startupStageName(StartupStage stage) {
    switch (stage) {
    case StartupStage::Created: return "created";
    case StartupStage::EntryHeld: return "entry-held";
    case StartupStage::Preparing: return "preparing";
    case StartupStage::ReceiverReady: return "receiver-ready";
    case StartupStage::EntryReleased: return "entry-released";
    case StartupStage::EntryAdvanced: return "entry-advanced";
    case StartupStage::DebugDetached: return "debug-detached";
    case StartupStage::Committed: return "committed";
    }
    return "unknown";
}

namespace {
DWORD observeResult(const char* operation, DWORD error, const std::string& args = {},
              wa::log::Level level = wa::log::Level::Debug) {
    WA_LOG(error == ERROR_SUCCESS ? level : wa::log::Level::Warn, "Startup", operation, args,
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    return error;
}
// Snapshot the thread's error inside the expression, before logging arguments
// can allocate or call Win32. Argument evaluation order cannot erase the error.
#define observe(operation, expression, ...) observeResult(operation, [&] { \
    const bool succeeded = (expression); \
    return succeeded ? DWORD(ERROR_SUCCESS) : GetLastError(); \
}(), __VA_ARGS__)
Result failure(DWORD error, const char* operation) {
    return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(error)), std::string("Startup: ") + operation);
}
void close(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
        observe("CloseHandle", CloseHandle(handle) != FALSE,
                "handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle)));
        handle = nullptr;
    }
}

struct Launch {
    const StartupRequest& request;
    const StartupPrepare& prepare;
    const std::atomic_bool* cancelled;
    const StartupProgress& progress;
    StartupOutcome out;
    HANDLE job = nullptr;
    PROCESS_INFORMATION child{};
    std::atomic_bool stop{false};
    std::atomic_bool prepared{false};
    Result preparation;
    std::thread installer;
    bool workerFinished = false;
    bool eventPending = false;
    DEBUG_EVENT event{};
    bool held = false;
    bool released = false;
    bool loaderBreakpoint = false;
    ULONGLONG deadline = 0;

    Launch(const StartupRequest& r, const StartupPrepare& p, const std::atomic_bool* c,
           const StartupProgress& s) : request(r), prepare(p), cancelled(c), progress(s) {}

    Result checkpoint(StartupStage stage) {
        WA_LOG(wa::log::Level::Info, "Startup", startupStageName(stage),
               "pid=" + std::to_string(out.pid), "entry-boundary only");
        if (progress) progress(stage, out);
        if (!out.committed && cancelled && cancelled->load())
            return failure(ERROR_CANCELLED, "cancelled before commit");
        if (!out.committed && GetTickCount64() >= deadline)
            return failure(ERROR_TIMEOUT, "startup deadline expired");
        return Result::Ok();
    }

    Result read(uintptr_t address, void* value, size_t bytes) {
        SIZE_T copied = 0;
        const BOOL ok = ReadProcessMemory(child.hProcess, reinterpret_cast<const void*>(address), value, bytes, &copied);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        observeResult("ReadProcessMemory", error,
            "address=" + std::to_string(address) + " bytes=" + std::to_string(bytes));
        return ok && copied == bytes ? Result::Ok() : failure(ok ? ERROR_PARTIAL_COPY : error, "read target image");
    }

    Result writeEntry(uint8_t value) {
        void* address = reinterpret_cast<void*>(out.entryAddress);
        DWORD old = 0;
        const DWORD protectError = observe("VirtualProtectEx(entry write)",
            VirtualProtectEx(child.hProcess, address, 1, PAGE_EXECUTE_READWRITE, &old) != FALSE,
            "address=" + std::to_string(out.entryAddress) + " bytes=1 PAGE_EXECUTE_READWRITE");
        if (protectError) return failure(protectError, "protect entry");
        SIZE_T written = 0;
        const BOOL wrote = WriteProcessMemory(child.hProcess, address, &value, 1, &written);
        const DWORD writeError = wrote ? ERROR_SUCCESS : GetLastError();
        observeResult("WriteProcessMemory(entry)", writeError,
                                        "value=" + std::to_string(value) + " bytes=1");
        const DWORD flushError = observe("FlushInstructionCache(entry)",
            FlushInstructionCache(child.hProcess, address, 1) != FALSE, "bytes=1");
        DWORD ignored = 0;
        const DWORD restoreError = observe("VirtualProtectEx(entry restore)",
            VirtualProtectEx(child.hProcess, address, 1, old, &ignored) != FALSE,
            "protection=" + std::to_string(old));
        if (!wrote || written != 1) return failure(writeError ? writeError : ERROR_PARTIAL_COPY, "write entry");
        if (flushError || restoreError) return failure(flushError ? flushError : restoreError, "restore entry protection/cache");
        return Result::Ok();
    }

    Result setBreakpoint() {
        out.imageBase = reinterpret_cast<uintptr_t>(event.u.CreateProcessInfo.lpBaseOfImage);
        IMAGE_DOS_HEADER dos{};
        Result result = read(out.imageBase, &dos, sizeof(dos));
        if (!result) return result;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 1024 * 1024)
            return failure(ERROR_BAD_EXE_FORMAT, "invalid PE header");
        IMAGE_NT_HEADERS nt{};
        result = read(out.imageBase + static_cast<uintptr_t>(dos.e_lfanew), &nt, sizeof(nt));
        if (!result) return result;
#ifdef _WIN64
        constexpr WORD magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        constexpr WORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
        constexpr WORD magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        constexpr WORD machine = IMAGE_FILE_MACHINE_I386;
#endif
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != magic || nt.FileHeader.Machine != machine ||
            !nt.OptionalHeader.AddressOfEntryPoint || nt.OptionalHeader.AddressOfEntryPoint >= nt.OptionalHeader.SizeOfImage)
            return failure(ERROR_BAD_EXE_FORMAT, "unsupported image bitness/entry");
        out.entryRva = nt.OptionalHeader.AddressOfEntryPoint;
        out.entryAddress = out.imageBase + out.entryRva;
        result = read(out.entryAddress, &out.originalByte, 1);
        if (!result) return result;
        if (out.originalByte == 0xCC) return failure(ERROR_NOT_SUPPORTED, "entry already contains a breakpoint");
        return writeEntry(0xCC);
    }

    Result continueEvent(DWORD disposition = DBG_CONTINUE) {
        const DWORD error = observe("ContinueDebugEvent", ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition) != FALSE,
            "pid=" + std::to_string(event.dwProcessId) + " tid=" + std::to_string(event.dwThreadId) +
            " disposition=" + std::to_string(disposition));
        if (error) return failure(error, "continue debug event");
        eventPending = false;
        return Result::Ok();
    }

    Result holdEntry() {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        DWORD error = observe("GetThreadContext(entry)", GetThreadContext(child.hThread, &context) != FALSE,
                              "CONTEXT_CONTROL");
        if (error) return failure(error, "read entry context");
#ifdef _WIN64
        if (context.Rip != out.entryAddress + 1) return failure(ERROR_INVALID_DATA, "unexpected entry instruction pointer");
        context.Rip = out.entryAddress;
#else
        if (context.Eip != out.entryAddress + 1) return failure(ERROR_INVALID_DATA, "unexpected entry instruction pointer");
        context.Eip = static_cast<DWORD>(out.entryAddress);
#endif
        if (context.EFlags & 0x100u) return failure(ERROR_NOT_SUPPORTED, "pre-existing single-step flag");
        Result result = writeEntry(out.originalByte);
        if (!result) return result;
        error = observe("SetThreadContext(entry rewind)", SetThreadContext(child.hThread, &context) != FALSE,
                        "entry=" + std::to_string(out.entryAddress));
        if (error) return failure(error, "rewind entry context");
        const DWORD previous = SuspendThread(child.hThread);
        error = previous == DWORD(-1) ? GetLastError() : ERROR_SUCCESS;
        observeResult("SuspendThread(entry gate)", error, "previous=" + std::to_string(previous));
        if (error) return failure(error, "hold entry thread");
        if (previous != 0) return failure(ERROR_BUSY, "entry thread already suspended");
        held = true;
        out.entryRestored = true;
        result = checkpoint(StartupStage::EntryHeld);
        if (!result) return result;
        result = continueEvent();
        if (!result) return result;
        result = checkpoint(StartupStage::Preparing);
        if (!result) return result;
        installer = std::thread([this] {
            try { preparation = prepare(out.pid, stop); }
            catch (const std::exception& e) { preparation = Result::Fail(static_cast<long>(E_FAIL), e.what()); }
            catch (...) { preparation = failure(ERROR_UNHANDLED_EXCEPTION, "preparation raised an exception"); }
            prepared.store(true, std::memory_order_release);
        });
        return Result::Ok();
    }

    Result releaseEntry() {
        Result result = checkpoint(StartupStage::ReceiverReady);
        if (!result) return result;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        DWORD error = observe("GetThreadContext(single-step)", GetThreadContext(child.hThread, &context) != FALSE, "CONTEXT_CONTROL");
        if (error) return failure(error, "read held context");
        context.EFlags |= 0x100u;
        error = observe("SetThreadContext(single-step)", SetThreadContext(child.hThread, &context) != FALSE, "set owned TF");
        if (error) return failure(error, "set entry single-step");
        const DWORD previous = ResumeThread(child.hThread);
        error = previous == DWORD(-1) ? GetLastError() : ERROR_SUCCESS;
        observeResult("ResumeThread(entry gate)", error, "previous=" + std::to_string(previous));
        if (error) return failure(error, "release entry thread");
        held = false;
        if (previous != 1) return failure(ERROR_BUSY, "entry suspension changed");
        released = true;
        return checkpoint(StartupStage::EntryReleased);
    }

    Result commit() {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        DWORD error = observe("GetThreadContext(after entry instruction)", GetThreadContext(child.hThread, &context) != FALSE,
                              "CONTEXT_CONTROL");
        if (error) return failure(error, "read advanced context");
        context.EFlags &= ~0x100u;
        error = observe("SetThreadContext(clear single-step)", SetThreadContext(child.hThread, &context) != FALSE, "clear owned TF");
        if (error) return failure(error, "clear entry single-step");
        out.entryAdvanced = true;
        Result result = continueEvent();
        if (!result) return result;
        result = checkpoint(StartupStage::EntryAdvanced);
        if (!result) return result;
        error = observe("DebugActiveProcessStop", DebugActiveProcessStop(out.pid) != FALSE,
                        "pid=" + std::to_string(out.pid));
        if (error) return failure(error, "detach startup debugger");
        out.debugDetached = true;
        result = checkpoint(StartupStage::DebugDetached);
        if (!result) return result;
        const DWORD alive = WaitForSingleObject(child.hProcess, 0);
        error = alive == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        observeResult("WaitForSingleObject(before commit)", error, "outcome=" + std::to_string(alive) + " timeoutMs=0");
        if (alive == WAIT_OBJECT_0) {
            out.targetExitConfirmed = true;
            return failure(ERROR_PROCESS_ABORTED, "target exited before commit");
        }
        if (alive != WAIT_TIMEOUT) return failure(error, "verify live target before commit");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION policy{};
        const BOOL committed = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &policy, sizeof(policy));
        error = committed ? ERROR_SUCCESS : GetLastError();
        // Publish the OS commit before logging, allocation, or a callback. If
        // delivery is lost after this point the target is already independent.
        if (committed) out.committed = true;
        observeResult("SetInformationJobObject(commit)", error, "clear KILL_ON_JOB_CLOSE; commit point");
        if (error) return failure(error, "commit independent target");
        out.result = Result::Ok();
        return checkpoint(StartupStage::Committed);
    }

    Result runDebugger() {
        deadline = GetTickCount64() + request.timeoutMs;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::wstring command = L"\"" + request.executable + L"\"";
        if (!request.arguments.empty()) command += L" " + request.arguments;
        const std::wstring directory = request.directory.empty()
            ? request.executable.substr(0, request.executable.find_last_of(L"\\/")) : request.directory;
        DWORD error = observe("CreateProcessW(debug startup)", CreateProcessW(request.executable.c_str(), command.data(), nullptr,
            nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
            directory.empty() ? nullptr : directory.c_str(), &startup, &child) != FALSE,
            "exe=" + wa::narrowAscii(request.executable) + " command=" + wa::narrowAscii(command) +
            " directory=" + wa::narrowAscii(directory) +
            " DEBUG_ONLY_THIS_PROCESS|CREATE_SUSPENDED|CREATE_NO_WINDOW; inherit=FALSE", wa::log::Level::Info);
        if (error) return failure(error, "create debug target");
        out.pid = child.dwProcessId;
        out.mainThreadId = child.dwThreadId;
        error = observe("DebugSetProcessKillOnExit", DebugSetProcessKillOnExit(TRUE) != FALSE, "TRUE; dedicated debugger thread");
        if (error) return failure(error, "set debugger exit policy");
        error = observe("AssignProcessToJobObject", AssignProcessToJobObject(job, child.hProcess) != FALSE,
                        "pid=" + std::to_string(out.pid));
        if (error) return failure(error, "protect uncommitted target");
        FILETIME created{}, exited{}, kernel{}, user{};
        error = observe("GetProcessTimes(startup)", GetProcessTimes(child.hProcess, &created, &exited, &kernel, &user) != FALSE,
                        "pid=" + std::to_string(out.pid));
        if (error) return failure(error, "identify startup target");
        out.creationTime = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        Result result = checkpoint(StartupStage::Created);
        if (!result) return result;
        const DWORD previous = ResumeThread(child.hThread);
        error = previous == DWORD(-1) ? GetLastError() : ERROR_SUCCESS;
        observeResult("ResumeThread(created suspension)", error, "previous=" + std::to_string(previous));
        if (error || previous != 1) return failure(error ? error : ERROR_BUSY, "restore creation suspension");
        while (!out.committed) {
            const DWORD alive = WaitForSingleObject(child.hProcess, 0);
            const DWORD aliveError = alive == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            observeResult("WaitForSingleObject(debug target)", aliveError,
                          "outcome=" + std::to_string(alive) + " timeoutMs=0");
            if (alive != WAIT_TIMEOUT) {
                if (alive == WAIT_OBJECT_0) {
                    out.targetExitConfirmed = true;
                    return failure(ERROR_PROCESS_ABORTED, "target exited before commit");
                }
                return failure(aliveError, "verify debug target state");
            }
            if (cancelled && cancelled->load()) return failure(ERROR_CANCELLED, "cancelled before commit");
            if (GetTickCount64() >= deadline) return failure(ERROR_TIMEOUT, "startup deadline expired");
            if (held && prepared.load(std::memory_order_acquire)) {
                if (!preparation) return preparation;
                result = releaseEntry();
                if (!result) return result;
            }
            const BOOL available = WaitForDebugEvent(&event, 10);
            const DWORD waitError = available ? ERROR_SUCCESS : GetLastError();
            if (!available) {
                if (waitError == ERROR_SEM_TIMEOUT) continue;
                observeResult("WaitForDebugEvent", waitError, "timeoutMs=10");
                return failure(waitError, "wait debug event");
            }
            eventPending = true;
            WA_LOG(wa::log::Level::Debug, "Startup", "debug event", "code=" + std::to_string(event.dwDebugEventCode) +
                   " tid=" + std::to_string(event.dwThreadId), "S_OK");
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
                close(event.u.CreateProcessInfo.hFile);
                // Windows owns the debug-event process/thread handles until
                // exit/detach. Closing them here can close a reused handle when
                // DebugActiveProcessStop performs its own handle cleanup.
                result = setBreakpoint();
                if (!result) return result;
            } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                close(event.u.LoadDll.hFile);
            } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
                out.targetExitConfirmed = true;
                result = continueEvent();
                return result ? failure(ERROR_PROCESS_ABORTED, "target exited before commit") : result;
            } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
                const auto& exception = event.u.Exception;
                const DWORD code = exception.ExceptionRecord.ExceptionCode;
                const uintptr_t address = reinterpret_cast<uintptr_t>(exception.ExceptionRecord.ExceptionAddress);
                if (code == EXCEPTION_BREAKPOINT && address == out.entryAddress && event.dwThreadId == child.dwThreadId && !held && !released) {
                    result = holdEntry();
                    if (!result) return result;
                    continue;
                }
                if (code == EXCEPTION_SINGLE_STEP && released && event.dwThreadId == child.dwThreadId) return commit();
                // Windows supplies one initial loader breakpoint before EXE entry.
                if (code == EXCEPTION_BREAKPOINT && !loaderBreakpoint && !held && !released &&
                    event.dwThreadId == child.dwThreadId && exception.dwFirstChance) loaderBreakpoint = true;
                else {
                    result = continueEvent(DBG_EXCEPTION_NOT_HANDLED);
                    if (!result) return result;
                    continue;
                }
            }
            result = continueEvent();
            if (!result) return result;
        }
        return Result::Ok();
    }

    void endUncommitted() {
        stop.store(true);
        if (!child.hProcess || out.committed) return;
        observe("TerminateProcess(uncommitted)", TerminateProcess(child.hProcess, ERROR_CANCELLED) != FALSE,
                "pid=" + std::to_string(out.pid), wa::log::Level::Info);
        if (out.debugDetached) return;
        const ULONGLONG end = GetTickCount64() + 5000;
        if (eventPending) continueEvent();
        while (GetTickCount64() < end && !out.targetExitConfirmed) {
            if (!WaitForDebugEvent(&event, 25)) continue;
            eventPending = true;
            if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) close(event.u.LoadDll.hFile);
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
                close(event.u.CreateProcessInfo.hFile);
            }
            if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) out.targetExitConfirmed = true;
            if (!continueEvent()) break;
        }
    }

    static DWORD WINAPI threadEntry(void* parameter) {
        auto& self = *static_cast<Launch*>(parameter);
        try { self.out.result = self.runDebugger(); }
        catch (const std::exception& e) { self.out.result = Result::Fail(static_cast<long>(E_FAIL), e.what()); }
        catch (...) { self.out.result = failure(ERROR_UNHANDLED_EXCEPTION, "debug controller fault"); }
        if (!self.out.committed) self.endUncommitted();
        else self.out.result = Result::Ok();
        self.workerFinished = true;
        return 0;
    }
};
}  // namespace

StartupOutcome launchAtEntry(const StartupRequest& request, const StartupPrepare& prepare,
                             const std::atomic_bool* cancelled, const StartupProgress& progress) {
    Launch launch(request, prepare, cancelled, progress);
    if (request.executable.empty() || !prepare || !request.timeoutMs || request.executable.find(L'"') != std::wstring::npos) {
        launch.out.result = failure(ERROR_INVALID_PARAMETER, "invalid startup request");
        return launch.out;
    }
    if (cancelled && cancelled->load()) {
        launch.out.result = failure(ERROR_CANCELLED, "cancelled before creation");
        return launch.out;
    }
    launch.job = CreateJobObjectW(nullptr, nullptr);
    DWORD error = launch.job ? ERROR_SUCCESS : GetLastError();
    observeResult("CreateJobObjectW(startup)", error, "unnamed; non-inherited");
    if (!error) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION policy{};
        policy.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        error = observe("SetInformationJobObject(guard)", SetInformationJobObject(launch.job, JobObjectExtendedLimitInformation,
            &policy, sizeof(policy)) != FALSE, "KILL_ON_JOB_CLOSE");
    }
    if (error) {
        launch.out.result = failure(error, "create startup lifetime guard");
        close(launch.job);
        return launch.out;
    }
    HANDLE worker = CreateThread(nullptr, 0, &Launch::threadEntry, &launch, 0, nullptr);
    error = worker ? ERROR_SUCCESS : GetLastError();
    observeResult("CreateThread(startup debugger)", error, "dedicated debugger ownership");
    if (!error) {
        const DWORD waited = WaitForSingleObject(worker, INFINITE);
        error = waited == WAIT_OBJECT_0 ? ERROR_SUCCESS : GetLastError();
        observeResult("WaitForSingleObject(startup debugger)", error, "INFINITE; caller may use background task");
        close(worker);
    }
    if (error) launch.out.result = failure(error, "startup debugger unavailable");
    if (!error && !launch.workerFinished && !launch.out.committed)
        launch.out.result = failure(ERROR_UNHANDLED_EXCEPTION, "debug controller ended unexpectedly");
    launch.stop.store(true);
    if (!launch.out.committed && launch.child.hProcess) {
        // This runs outside the debugger thread, including if it used ExitThread
        // after detach. The job is not owned by that thread's stack unwinding.
        observe("TerminateJobObject(uncommitted)", TerminateJobObject(launch.job, ERROR_CANCELLED) != FALSE,
                "pid=" + std::to_string(launch.out.pid), wa::log::Level::Info);
        const DWORD waited = WaitForSingleObject(launch.child.hProcess, 5000);
        const DWORD waitError = waited == WAIT_OBJECT_0 ? ERROR_SUCCESS : waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        observeResult("WaitForSingleObject(uncommitted target)", waitError, "timeoutMs=5000");
        launch.out.targetExitConfirmed = waited == WAIT_OBJECT_0;
    }
    if (launch.installer.joinable()) launch.installer.join();
    close(launch.job);
    close(launch.child.hThread);
    close(launch.child.hProcess);
    return launch.out;
}
}  // namespace wa
