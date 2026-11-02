#include <gtest/gtest.h>
#include "RemoteInjection.h"
#include "RemoteInjectionHarness.h"
#include "OnDemandAttach.h"
#include "HookedCallPod.h"
#include "HookModuleIdentity.h"
#include <tlhelp32.h>
#include <future>
#include <filesystem>
#include <string>
#include <fstream>

namespace {
TEST(RemoteThreadWait, InvalidHandlePreservesWaitFailedSystemError) {
    SetLastError(ERROR_ACCESS_DENIED);
    const auto result = wa::waitRemoteThread(nullptr, 0);
    EXPECT_EQ(result.outcome, static_cast<uint32_t>(WAIT_FAILED));
    EXPECT_EQ(result.error, static_cast<uint32_t>(ERROR_INVALID_HANDLE));
}

class RemoteInjectionTest : public testing::Test {
protected:
    virtual bool holdLoader() const { return false; }
    void SetUp() override {
        static uint32_t serial = 0;
        base = L"Local\\WinAudio-RemoteTest-" + std::to_wstring(GetCurrentProcessId()) +
               L"-" + std::to_wstring(++serial);
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    sizeof(wa::injection_probe::Shared), (base + L"-map").c_str());
        ASSERT_NE(mapping, nullptr);
        shared = static_cast<wa::injection_probe::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        ASSERT_NE(shared, nullptr);
        gate = CreateEventW(nullptr, TRUE, FALSE, (base + L"-gate").c_str());
        entered = CreateEventW(nullptr, TRUE, FALSE, (base + L"-entered").c_str());
        ready = CreateEventW(nullptr, TRUE, FALSE, (base + L"-ready").c_str());
        stop = CreateEventW(nullptr, TRUE, FALSE, (base + L"-stop").c_str());
        ASSERT_TRUE(gate && entered && ready && stop);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring exe = path;
        exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"RemoteInjectionTarget.exe";
        std::wstring command = L"\"" + exe + L"\" \"" + base + L"\"";
        if (holdLoader()) command += L" --hold-loader";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        ASSERT_TRUE(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
        ASSERT_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
        if (holdLoader()) ASSERT_EQ(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
    }
    void TearDown() override {
        if (gate) SetEvent(gate);
        if (stop) SetEvent(stop);
        if (child.hProcess) {
            const DWORD waited = WaitForSingleObject(child.hProcess, 5000);
            EXPECT_EQ(waited, WAIT_OBJECT_0);
            if (waited != WAIT_OBJECT_0) {
                // Only the fixture's own child is terminated after a failed
                // graceful teardown, never an application's attach target.
                TerminateProcess(child.hProcess, 91);
                WaitForSingleObject(child.hProcess, 5000);
            }
            DWORD exit = STILL_ACTIVE;
            EXPECT_TRUE(GetExitCodeProcess(child.hProcess, &exit));
            EXPECT_EQ(exit, 0u);
            CloseHandle(child.hProcess);
        }
        if (child.hThread) CloseHandle(child.hThread);
        if (shared) UnmapViewOfFile(shared);
        for (HANDLE handle : {mapping, gate, entered, ready, stop}) if (handle) CloseHandle(handle);
    }
    void begin(wa::RemoteInjection& operation) {
        const uint32_t value = wa::injection_probe::kValue;
        ASSERT_TRUE(operation.begin(child.hProcess,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(shared->entry), &value, sizeof(value)));
        ASSERT_EQ(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
    }
    void releaseAndCheck(wa::RemoteInjection& operation) {
        const void* address = operation.parameter();
        SetEvent(gate);
        ASSERT_TRUE(operation.wait(5000));
        EXPECT_EQ(operation.exitCode(), 0x7351u);
        EXPECT_EQ(shared->readOk, 1u);
        EXPECT_EQ(shared->value, wa::injection_probe::kValue);
        MEMORY_BASIC_INFORMATION memory{};
        ASSERT_NE(VirtualQueryEx(child.hProcess, address, &memory, sizeof(memory)), 0u);
        EXPECT_EQ(memory.State, static_cast<DWORD>(MEM_FREE));
    }
    void missingHookProbe(bool late = false) {
        wchar_t executable[MAX_PATH] = {}, temporary[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        GetTempPathW(MAX_PATH, temporary);
        const auto source = std::filesystem::path(executable).parent_path() / L"RemoteInjectionTarget.exe";
        const auto folder = std::filesystem::path(temporary) /
            (L"WinAudio-MissingHook-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directory(folder);
        const auto copied = folder / L"RemoteInjectionTarget.exe";
        std::filesystem::copy_file(source, copied, std::filesystem::copy_options::overwrite_existing);
        // A real, verifiable source is staged first. LoadLibrary then rejects
        // this deliberately invalid PE, including after a cancelled wait.
        std::ofstream(folder / L"WinAudioHook.dll", std::ios::binary | std::ios::trunc)
            << "WinAudio test invalid PE";
        std::wstring command = L"\"" + copied.wstring() + L"\" --missing-hook " + std::to_wstring(child.dwProcessId);
        if (late) command += L" \"" + base + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION probe{};
        ASSERT_TRUE(CreateProcessW(copied.c_str(), command.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &probe));
        if (late) {
            const ULONGLONG deadline = GetTickCount64() + 5000;
            while (ReadULongAcquire(reinterpret_cast<volatile ULONG*>(&shared->readOk)) == 0 &&
                   GetTickCount64() < deadline) Sleep(1);
            EXPECT_EQ(shared->readOk, 1u);
            SetEvent(gate);
        }
        const DWORD waited = WaitForSingleObject(probe.hProcess, 7000);
        EXPECT_EQ(waited, WAIT_OBJECT_0);
        if (waited != WAIT_OBJECT_0) {
            TerminateProcess(probe.hProcess, 92);
            WaitForSingleObject(probe.hProcess, 5000);
        }
        DWORD code = STILL_ACTIVE;
        EXPECT_TRUE(GetExitCodeProcess(probe.hProcess, &code));
        EXPECT_EQ(code, 0u);
        CloseHandle(probe.hThread);
        CloseHandle(probe.hProcess);
        std::filesystem::remove(copied);
        std::filesystem::remove(folder / L"WinAudioHook.dll");
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            const auto name = entry.path().filename().wstring();
            if (name.rfind(L"WinAudioHook-", 0) == 0 && entry.path().extension() == L".dll")
                std::filesystem::remove(entry.path());
        }
        std::filesystem::remove(folder);
    }
    std::wstring base;
    PROCESS_INFORMATION child{};
    HANDLE mapping = nullptr, gate = nullptr, entered = nullptr, ready = nullptr, stop = nullptr;
    wa::injection_probe::Shared* shared = nullptr;
};

TEST_F(RemoteInjectionTest, TimeoutKeepsUnreadRemoteParameterAlive) {
    wa::RemoteInjection operation;
    begin(operation);
    const void* address = operation.parameter();
    const auto result = operation.wait(10);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
    MEMORY_BASIC_INFORMATION memory{};
    ASSERT_NE(VirtualQueryEx(child.hProcess, address, &memory, sizeof(memory)), 0u);
    EXPECT_EQ(memory.State, static_cast<DWORD>(MEM_COMMIT));
    releaseAndCheck(operation);
}

TEST_F(RemoteInjectionTest, CancellationKeepsUnreadRemoteParameterAlive) {
    wa::RemoteInjection operation;
    begin(operation);
    std::atomic_bool cancelled{true};
    const auto result = operation.wait(5000, &cancelled);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
    releaseAndCheck(operation);
}

TEST_F(RemoteInjectionTest, FailedLoadDoesNotPermanentlyReserveTarget) {
    missingHookProbe();
}

TEST_F(RemoteInjectionTest, TargetExitReclaimsPendingOperation) {
    wa::RemoteInjection operation;
    begin(operation);
    EXPECT_FALSE(operation.wait(1));
    SetEvent(stop);
    ASSERT_EQ(WaitForSingleObject(child.hProcess, 5000), WAIT_OBJECT_0);
    EXPECT_TRUE(operation.poll());
    EXPECT_FALSE(operation.pending());
    EXPECT_EQ(operation.parameter(), nullptr);
    EXPECT_EQ(operation.completionResult().code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED)));
}

TEST_F(RemoteInjectionTest, ExistingIncompatibleHookIsRejectedBeforeLoadingAnotherDll) {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto directory = std::filesystem::path(self).parent_path();
    const std::wstring obsolete = (directory / L"WinAudioHook-obsolete.dll").wstring();
    const auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    ASSERT_NE(load, nullptr);
    wa::RemoteInjection preload;
    ASSERT_TRUE(preload.begin(child.hProcess, reinterpret_cast<LPTHREAD_START_ROUTINE>(load),
                             obsolete.c_str(), (obsolete.size() + 1) * sizeof(wchar_t)));
    ASSERT_TRUE(preload.wait(5000));
    bool loaded = false;
    ASSERT_TRUE(wa::inspectHookModules(child.dwProcessId, obsolete, loaded));
    ASSERT_TRUE(loaded);
    wa::OnDemandAttach attach;
    const auto result = attach.start(child.dwProcessId);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)));
    EXPECT_NE(result.message.find("restart"), std::string::npos);
    EXPECT_FALSE(attach.attached());
    // All WinAudioHook-named modules are inspected, even when this layout's
    // mapping does not exist. Only the fixture may remain loaded afterwards.
    loaded = false;
    EXPECT_TRUE(wa::inspectHookModules(child.dwProcessId, obsolete, loaded));
    EXPECT_TRUE(loaded);
}

TEST_F(RemoteInjectionTest, ControllerOwnerDestructionDoesNotFreeUnreadParameter) {
    const void* address = nullptr;
    {
        wa::RemoteInjection operation;
        begin(operation);
        address = operation.parameter();
        EXPECT_FALSE(operation.wait(1));
    }
    MEMORY_BASIC_INFORMATION memory{};
    ASSERT_NE(VirtualQueryEx(child.hProcess, address, &memory, sizeof(memory)), 0u);
    EXPECT_EQ(memory.State, static_cast<DWORD>(MEM_COMMIT));
    SetEvent(gate);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (ReadULongAcquire(reinterpret_cast<volatile ULONG*>(&shared->readOk)) == 0 &&
           GetTickCount64() < deadline) Sleep(1);
    EXPECT_EQ(shared->readOk, 1u);
    EXPECT_EQ(shared->value, wa::injection_probe::kValue);
    // The intentionally abandoned shutdown allocation belongs to the target;
    // target exit, rather than a controller destructor, reclaims it.
}

uint32_t threadCount(uint32_t pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    uint32_t count = 0;
    for (BOOL have = Thread32First(snapshot, &entry); have; have = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == pid) ++count;
    CloseHandle(snapshot);
    return count;
}

class AttachLifecycleTest : public RemoteInjectionTest {
protected:
    bool holdLoader() const override { return true; }

    void exercisePending(bool cancel) {
        wa::OnDemandAttach attach;
        std::atomic_bool cancelled{false};
        const uint32_t initialThreads = threadCount(child.dwProcessId);
        auto started = std::async(std::launch::async, [&] {
            return attach.start(child.dwProcessId, {}, wa::PipelineFlow::Render, &cancelled);
        });
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (threadCount(child.dwProcessId) <= initialThreads && GetTickCount64() < deadline) Sleep(1);
        const bool remoteStarted = threadCount(child.dwProcessId) > initialThreads;
        if (!remoteStarted) SetEvent(gate);
        EXPECT_TRUE(remoteStarted);
        if (cancel) cancelled.store(true);
        const auto result = started.get();
        ASSERT_FALSE(result);
        EXPECT_EQ(result.code, static_cast<long>(HRESULT_FROM_WIN32(cancel ? ERROR_CANCELLED : ERROR_TIMEOUT)));
        EXPECT_NE(result.message.find("pending"), std::string::npos);
        attach.stop();
        attach.stop();
        EXPECT_EQ(wa::OnDemandAttach::reapPending(), 1u);
        wa::OnDemandAttach retry;
        const auto rejected = retry.start(child.dwProcessId);
        EXPECT_FALSE(rejected);
        EXPECT_EQ(rejected.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)));

        wchar_t name[64] = {};
        wa::hook_ipc::mapName(child.dwProcessId, name, 64);
        HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        ASSERT_NE(map, nullptr);
        auto* ring = static_cast<wa::hook_ipc::Ring*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        ASSERT_NE(ring, nullptr);
        EXPECT_EQ(ring->captureEnabled, 0u);
        SetEvent(gate);
        const ULONGLONG installedDeadline = GetTickCount64() + 5000;
        while (ReadULongAcquire(reinterpret_cast<volatile ULONG*>(&ring->installed)) == wa::hook_ipc::kInstallPending &&
               GetTickCount64() < installedDeadline) Sleep(1);
        EXPECT_NE(ring->installed, wa::hook_ipc::kInstallPending);
        EXPECT_EQ(ring->captureEnabled, 0u);  // Late HookThread must not re-enable capture.
        EXPECT_EQ(wa::OnDemandAttach::reapPending(), 0u);  // Maintenance, without another attach, reclaims it.
        UnmapViewOfFile(ring);
        CloseHandle(map);
        const auto retried = retry.start(child.dwProcessId, {}, wa::PipelineFlow::Render);
        if (retried) EXPECT_TRUE(retry.attached());
        else {
            EXPECT_EQ(retried.code, static_cast<long>(E_FAIL));
            EXPECT_EQ(retried.message, wa::attachInstallFailMessage(wa::hook_ipc::kInstallFailed));
            EXPECT_FALSE(retry.attached());
        }
        retry.stop();
        retry.stop();
        EXPECT_EQ(wa::OnDemandAttach::reapPending(), 0u);
    }
};

TEST_F(AttachLifecycleTest, CancellationRetainsRealInjectionAndPreventsLateCapture) {
    exercisePending(true);
}

TEST_F(AttachLifecycleTest, TimeoutRetainsRealInjectionAndPreventsLateCapture) {
    exercisePending(false);
}

TEST_F(AttachLifecycleTest, CancelledLoadThatLaterFailsIsReaped) {
    missingHookProbe(true);
}
}
