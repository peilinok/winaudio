#include <gtest/gtest.h>
#include "StartupLaunch.h"
#include "StartupGateHarness.h"
#include "RemoteInjection.h"
#include <windows.h>
#include <string>

namespace {
class StartupLaunchTest : public testing::Test {
protected:
    void SetUp() override {
        static unsigned serial = 0;
        token = L"Local\\WinAudio-Startup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial);
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                 sizeof(wa::startup_probe::Shared), (token + L"-map").c_str());
        ASSERT_NE(map, nullptr);
        shared = static_cast<wa::startup_probe::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        ASSERT_NE(shared, nullptr);
        ready = CreateEventW(nullptr, TRUE, FALSE, (token + L"-ready").c_str());
        stop = CreateEventW(nullptr, TRUE, FALSE, (token + L"-stop").c_str());
        checkpoint = CreateEventW(nullptr, TRUE, FALSE, (token + L"-checkpoint").c_str());
        release = CreateEventW(nullptr, TRUE, FALSE, (token + L"-release").c_str());
        exitRequest = CreateEventW(nullptr, TRUE, FALSE, (token + L"-exit").c_str());
        ASSERT_TRUE(ready && stop && checkpoint && release && exitRequest);
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        request.executable = path;
        request.executable.resize(request.executable.find_last_of(L"\\/") + 1);
        request.executable += L"StartupGateTarget.exe";
        request.arguments = L"--token=" + token;
    }
    void TearDown() override {
        if (stop) SetEvent(stop);
        if (process) {
            const DWORD waited = WaitForSingleObject(process, 5000);
            if (waited != WAIT_OBJECT_0) { TerminateProcess(process, 90); WaitForSingleObject(process, 5000); }
            CloseHandle(process);
        }
        if (shared) UnmapViewOfFile(shared);
        for (HANDLE h : {map, ready, stop, checkpoint, release, exitRequest}) if (h) CloseHandle(h);
    }
    wa::StartupOutcome launch(const wa::StartupPrepare& prepare, const wa::StartupProgress& progress = {},
                              const std::atomic_bool* cancel = nullptr) {
        return wa::launchAtEntry(request, prepare, cancel, [&](wa::StartupStage stage, const wa::StartupOutcome& out) {
            if (!process && out.pid) process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                                          FALSE, out.pid);
            if (progress) progress(stage, out);
        });
    }
    std::wstring token;
    wa::StartupRequest request;
    HANDLE map = nullptr, ready = nullptr, stop = nullptr, process = nullptr;
    HANDLE checkpoint = nullptr, release = nullptr, exitRequest = nullptr;
    wa::startup_probe::Shared* shared = nullptr;
};

TEST_F(StartupLaunchTest, HoldsRealAslrEntryUntilPreparationThenRunsRestoredImageIndependently) {
    bool prepared = false;
    const auto out = launch([&](uint32_t pid, const std::atomic_bool&) {
        EXPECT_EQ(shared->crtEntered, 0u);
        EXPECT_EQ(shared->mainEntered, 0u);
        EXPECT_NE(pid, 0u);
        prepared = true;
        return wa::Result::Ok();
    });
    ASSERT_TRUE(out.result) << out.result.message;
    EXPECT_TRUE(prepared);
    EXPECT_TRUE(out.committed);
    EXPECT_TRUE(out.entryRestored);
    EXPECT_TRUE(out.entryAdvanced);
    EXPECT_TRUE(out.debugDetached);
    ASSERT_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
    EXPECT_EQ(shared->imageBase, out.imageBase);
    EXPECT_EQ(shared->entryRva, out.entryRva);
    EXPECT_EQ(out.entryAddress, shared->imageBase + shared->entryRva);
    EXPECT_EQ(shared->entryByte, out.originalByte);
    EXPECT_EQ(shared->crtEntered, 1u);
    EXPECT_EQ(shared->mainEntered, 1u);
    EXPECT_EQ(WaitForSingleObject(process, 0), WAIT_TIMEOUT);
}

TEST_F(StartupLaunchTest, RemoteLoaderRunsWhileExeEntryRemainsHeld) {
    const auto out = launch([&](uint32_t pid, const std::atomic_bool& cancelled) {
        HANDLE target = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                    PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
        if (!target) return wa::Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(GetLastError())), "OpenProcess");
        wa::RemoteInjection injection;
        constexpr wchar_t library[] = L"version.dll";
        auto result = injection.begin(target, reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")), library, sizeof(library));
        if (result) result = injection.wait(5000, &cancelled);
        EXPECT_EQ(shared->crtEntered, 0u);
        EXPECT_EQ(shared->mainEntered, 0u);
        EXPECT_FALSE(injection.pending());
        CloseHandle(target);
        return result;
    });
    ASSERT_TRUE(out.result) << out.result.message;
    ASSERT_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
}

TEST_F(StartupLaunchTest, CancellationAtEveryUncommittedCheckpointConfirmsTargetExit) {
    for (const auto selected : {wa::StartupStage::Created, wa::StartupStage::EntryHeld, wa::StartupStage::Preparing,
             wa::StartupStage::ReceiverReady, wa::StartupStage::EntryReleased, wa::StartupStage::EntryAdvanced,
             wa::StartupStage::DebugDetached}) {
        SCOPED_TRACE(wa::startupStageName(selected));
        std::atomic_bool cancel{false};
        const auto out = launch([](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); },
            [&](wa::StartupStage stage, const wa::StartupOutcome&) { if (stage == selected) cancel.store(true); }, &cancel);
        EXPECT_FALSE(out.result);
        EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
        EXPECT_FALSE(out.committed);
        EXPECT_TRUE(out.targetExitConfirmed);
        ASSERT_NE(process, nullptr);
        EXPECT_EQ(WaitForSingleObject(process, 0), WAIT_OBJECT_0);
        CloseHandle(process);
        process = nullptr;
    }
}

TEST_F(StartupLaunchTest, AbruptDebuggerThreadExitAlsoCleansTheDetachedWindow) {
    for (const auto selected : {wa::StartupStage::Created, wa::StartupStage::EntryHeld, wa::StartupStage::Preparing,
             wa::StartupStage::ReceiverReady, wa::StartupStage::EntryReleased, wa::StartupStage::EntryAdvanced,
             wa::StartupStage::DebugDetached}) {
        SCOPED_TRACE(wa::startupStageName(selected));
        bool reached = false;
        const auto out = launch([](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); },
            [&](wa::StartupStage stage, const wa::StartupOutcome&) {
                if (stage == selected) { reached = true; ExitThread(71); }
            });
        EXPECT_TRUE(reached);
        EXPECT_FALSE(out.result);
        EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_UNHANDLED_EXCEPTION)));
        EXPECT_FALSE(out.committed);
        EXPECT_TRUE(out.targetExitConfirmed);
        ASSERT_NE(process, nullptr);
        EXPECT_EQ(WaitForSingleObject(process, 0), WAIT_OBJECT_0);
        CloseHandle(process);
        process = nullptr;
    }
}

TEST_F(StartupLaunchTest, PreparationTimeoutCancelsAndConfirmsExitWithoutReleasingEntry) {
    request.timeoutMs = 1500;
    bool entered = false;
    const auto out = launch([&](uint32_t, const std::atomic_bool& cancelled) {
        entered = true;
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (!cancelled.load()) WaitForSingleObject(event, 10);
        CloseHandle(event);
        return wa::Result::Ok();
    });
    EXPECT_TRUE(entered);
    EXPECT_FALSE(out.result);
    EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
    EXPECT_TRUE(out.targetExitConfirmed);
    EXPECT_FALSE(out.entryAdvanced);
    EXPECT_EQ(shared->crtEntered, 0u);
}

TEST_F(StartupLaunchTest, PreparationFailureNeverReleasesApplicationEntry) {
    const auto out = launch([](uint32_t, const std::atomic_bool&) {
        return wa::Result::Fail(static_cast<long>(E_ACCESSDENIED), "controlled readiness failure");
    });
    EXPECT_FALSE(out.result);
    EXPECT_EQ(out.result.code, static_cast<long>(E_ACCESSDENIED));
    EXPECT_TRUE(out.targetExitConfirmed);
    EXPECT_FALSE(out.entryAdvanced);
    EXPECT_EQ(shared->crtEntered, 0u);
}

TEST_F(StartupLaunchTest, TargetExitDuringPreparationIsNotSuccessfulStartup) {
    const auto out = launch([](uint32_t pid, const std::atomic_bool&) {
        HANDLE target = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        const BOOL killed = TerminateProcess(target, 72);
        CloseHandle(target);
        return killed ? wa::Result::Ok() : wa::Result::Fail(static_cast<long>(E_FAIL), "terminate controlled child");
    });
    EXPECT_FALSE(out.result);
    EXPECT_TRUE(out.targetExitConfirmed);
    EXPECT_FALSE(out.committed);
}

TEST_F(StartupLaunchTest, ResponseLossAfterCommitLeavesIndependentTargetRunning) {
    const auto out = launch([](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); },
        [](wa::StartupStage stage, const wa::StartupOutcome&) { if (stage == wa::StartupStage::Committed) ExitThread(73); });
    ASSERT_TRUE(out.result) << out.result.message;
    EXPECT_TRUE(out.committed);
    EXPECT_TRUE(out.debugDetached);
    ASSERT_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
    EXPECT_EQ(WaitForSingleObject(process, 0), WAIT_TIMEOUT);
}

TEST_F(StartupLaunchTest, PreCancelledRequestCreatesNoTarget) {
    std::atomic_bool cancel{true};
    const auto out = launch([](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); }, {}, &cancel);
    EXPECT_FALSE(out.result);
    EXPECT_EQ(out.pid, 0u);
    EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
}

TEST_F(StartupLaunchTest, TargetCanExitItselfWhileTheExeEntryIsHeld) {
    request.arguments += L" --exit-worker";
    const auto out = launch([&](uint32_t, const std::atomic_bool&) {
        EXPECT_EQ(shared->crtEntered, 0u);
        SetEvent(exitRequest);
        const DWORD waited = WaitForSingleObject(process, 5000);
        return waited == WAIT_OBJECT_0 ? wa::Result::Ok() : wa::Result::Fail(static_cast<long>(E_FAIL), "self-exit timed out");
    });
    EXPECT_FALSE(out.result);
    EXPECT_FALSE(out.committed);
    EXPECT_TRUE(out.targetExitConfirmed);
    EXPECT_EQ(shared->crtEntered, 0u);
    DWORD code = STILL_ACTIVE;
    ASSERT_TRUE(GetExitCodeProcess(process, &code));
    EXPECT_EQ(code, 75u);
}

TEST_F(StartupLaunchTest, TargetSelfExitInDetachedWindowCannotCommitAnActiveTarget) {
    request.arguments += L" --exit-worker";
    const auto out = launch([](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); },
        [&](wa::StartupStage stage, const wa::StartupOutcome&) {
            if (stage == wa::StartupStage::DebugDetached) {
                SetEvent(exitRequest);
                EXPECT_EQ(WaitForSingleObject(process, 5000), WAIT_OBJECT_0);
            }
        });
    EXPECT_FALSE(out.result);
    EXPECT_FALSE(out.committed);
    EXPECT_TRUE(out.debugDetached);
    EXPECT_TRUE(out.targetExitConfirmed);
    DWORD code = STILL_ACTIVE;
    ASSERT_TRUE(GetExitCodeProcess(process, &code));
    EXPECT_EQ(code, 75u);
}

TEST_F(StartupLaunchTest, OriginalTargetBreakpointBeforeExeEntryIsNotSwallowed) {
    request.arguments += L" --debugbreak-before-entry";
    const auto out = launch([](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); });
    EXPECT_FALSE(out.result);
    EXPECT_FALSE(out.committed);
    EXPECT_TRUE(out.targetExitConfirmed);
    DWORD code = STILL_ACTIVE;
    ASSERT_TRUE(GetExitCodeProcess(process, &code));
    EXPECT_EQ(code, static_cast<DWORD>(EXCEPTION_BREAKPOINT));
}

TEST_F(StartupLaunchTest, ForeignSuspensionIsNotSilentlyDrainedDuringRelease) {
    const auto out = launch([](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); },
        [](wa::StartupStage stage, const wa::StartupOutcome& snapshot) {
            if (stage != wa::StartupStage::ReceiverReady) return;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, snapshot.mainThreadId);
            ASSERT_NE(thread, nullptr);
            EXPECT_EQ(SuspendThread(thread), 1u);
            CloseHandle(thread);
        });
    EXPECT_FALSE(out.result);
    EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)));
    EXPECT_TRUE(out.targetExitConfirmed);
    EXPECT_FALSE(out.committed);
}

TEST_F(StartupLaunchTest, NormalControllerProcessExitLeavesCommittedTargetIndependent) {
    std::wstring probe = request.executable;
    probe.resize(probe.find_last_of(L"\\/") + 1);
    probe += L"StartupGateProbe.exe";
    std::wstring command = L"\"" + probe + L"\" --controller " + token + L" -1";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION controller{};
    ASSERT_TRUE(CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &controller));
    const DWORD waited = WaitForSingleObject(controller.hProcess, 10000);
    EXPECT_EQ(waited, WAIT_OBJECT_0);
    if (waited != WAIT_OBJECT_0) { TerminateProcess(controller.hProcess, 82); WaitForSingleObject(controller.hProcess, 5000); }
    DWORD code = STILL_ACTIVE;
    EXPECT_TRUE(GetExitCodeProcess(controller.hProcess, &code));
    EXPECT_EQ(code, 0u);
    CloseHandle(controller.hThread);
    CloseHandle(controller.hProcess);
    process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, shared->targetPid);
    ASSERT_NE(process, nullptr);
    EXPECT_EQ(WaitForSingleObject(ready, 0), WAIT_OBJECT_0);
    EXPECT_EQ(WaitForSingleObject(process, 0), WAIT_TIMEOUT);
    SetEvent(stop);
    EXPECT_EQ(WaitForSingleObject(process, 5000), WAIT_OBJECT_0);
    EXPECT_TRUE(GetExitCodeProcess(process, &code));
    EXPECT_EQ(code, 0u);
}

TEST_F(StartupLaunchTest, TimeoutAtEachUncommittedCheckpointConfirmsTargetExit) {
    HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(never, nullptr);
    for (const auto selected : {wa::StartupStage::Created, wa::StartupStage::EntryHeld, wa::StartupStage::Preparing,
             wa::StartupStage::ReceiverReady, wa::StartupStage::EntryReleased, wa::StartupStage::EntryAdvanced,
             wa::StartupStage::DebugDetached}) {
        SCOPED_TRACE(wa::startupStageName(selected));
        request.timeoutMs = 1000;
        bool reached = false;
        const auto out = launch([](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); },
            [&](wa::StartupStage stage, const wa::StartupOutcome&) {
                if (stage == selected) { reached = true; WaitForSingleObject(never, 1100); }
            });
        EXPECT_TRUE(reached);
        EXPECT_FALSE(out.result);
        EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
        EXPECT_TRUE(out.targetExitConfirmed);
        EXPECT_FALSE(out.committed);
        if (process) { CloseHandle(process); process = nullptr; }
    }
    CloseHandle(never);
}

TEST_F(StartupLaunchTest, ForcedToolExitAtEachCheckpointKillsOnlyUncommittedTargets) {
    for (int selected = 0; selected <= static_cast<int>(wa::StartupStage::Committed); ++selected) {
        SCOPED_TRACE(selected);
        ResetEvent(checkpoint);
        ResetEvent(release);
        ResetEvent(stop);
        ResetEvent(ready);
        ZeroMemory(shared, sizeof(*shared));
        std::wstring probe = request.executable;
        probe.resize(probe.find_last_of(L"\\/") + 1);
        probe += L"StartupGateProbe.exe";
        std::wstring command = L"\"" + probe + L"\" --controller " + token + L" " + std::to_wstring(selected);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION controller{};
        ASSERT_TRUE(CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &startup, &controller));
        const DWORD reached = WaitForSingleObject(checkpoint, 10000);
        EXPECT_EQ(reached, WAIT_OBJECT_0);
        if (reached == WAIT_OBJECT_0) {
            EXPECT_EQ(shared->controllerStage, static_cast<uint32_t>(selected + 1));
            process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, shared->targetPid);
            EXPECT_NE(process, nullptr);
            if (process) EXPECT_EQ(WaitForSingleObject(process, 0), WAIT_TIMEOUT);
        }
        EXPECT_TRUE(TerminateProcess(controller.hProcess, 81));
        EXPECT_EQ(WaitForSingleObject(controller.hProcess, 5000), WAIT_OBJECT_0);
        DWORD controllerCode = STILL_ACTIVE;
        EXPECT_TRUE(GetExitCodeProcess(controller.hProcess, &controllerCode));
        EXPECT_EQ(controllerCode, 81u);
        if (process) {
            const bool committed = selected == static_cast<int>(wa::StartupStage::Committed);
            if (committed) {
                EXPECT_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
                EXPECT_EQ(WaitForSingleObject(process, 0), WAIT_TIMEOUT);
                SetEvent(stop);
            }
            const DWORD waited = WaitForSingleObject(process, 5000);
            EXPECT_EQ(waited, WAIT_OBJECT_0);
            DWORD code = STILL_ACTIVE;
            EXPECT_TRUE(GetExitCodeProcess(process, &code));
            if (committed) EXPECT_EQ(code, 0u);
            std::printf("force_tool_exit checkpoint=%d target_pid=%u controller_exit=%lu target_exit=%lu committed=%d\n",
                        selected, shared->targetPid, controllerCode, code, committed);
            if (waited != WAIT_OBJECT_0) { TerminateProcess(process, 90); WaitForSingleObject(process, 5000); }
            CloseHandle(process);
            process = nullptr;
        }
        CloseHandle(controller.hThread);
        CloseHandle(controller.hProcess);
    }
}
}  // namespace
