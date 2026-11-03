#include <gtest/gtest.h>
#include "RemoteInjection.h"
#include "RemoteInjectionHarness.h"
#include "OnDemandAttach.h"
#include "PipelineAttachController.h"
#include "PipelineAttachUiModel.h"
#include "CallLogExportJob.h"
#include "HookedCallPod.h"
#include "HookModuleIdentity.h"
#include "StartupGateHarness.h"
#include <tlhelp32.h>
#include <future>
#include <filesystem>
#include <string>
#include <fstream>
#include <cstdio>

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
        for (auto& sibling : siblings) {
            EXPECT_EQ(WaitForSingleObject(sibling.hProcess, 5000), WAIT_OBJECT_0);
            DWORD code = STILL_ACTIVE;
            EXPECT_TRUE(GetExitCodeProcess(sibling.hProcess, &code));
            std::printf("owned_target_exit pid=%lu code=%lu\n", sibling.dwProcessId, code);
            EXPECT_EQ(code, 0u);
            CloseHandle(sibling.hProcess);
            CloseHandle(sibling.hThread);
        }
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
            std::printf("owned_target_exit pid=%lu code=%lu\n", child.dwProcessId, exit);
            CloseHandle(child.hProcess);
        }
        if (child.hThread) CloseHandle(child.hThread);
        if (shared) UnmapViewOfFile(shared);
        for (HANDLE handle : {mapping, gate, entered, ready, stop}) if (handle) CloseHandle(handle);
    }
    PROCESS_INFORMATION spawnSibling(bool exitOnGo = false) {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const auto exe = std::filesystem::path(self).parent_path() / L"RemoteInjectionTarget.exe";
        std::wstring command = L"\"" + exe.wstring() + L"\" \"" + base + L"\"";
        if (exitOnGo) command += L" --exit-on-go";
        ResetEvent(ready);
        PROCESS_INFORMATION process{};
        STARTUPINFOW startup{sizeof(startup)};
        EXPECT_TRUE(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
        if (process.hProcess) {
            siblings.push_back(process);
            EXPECT_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
        }
        return process;
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
    std::vector<PROCESS_INFORMATION> siblings;
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

TEST_F(RemoteInjectionTest, StartupWaitingDoesNotBlockExistingConfigurationBatchOrDetach) {
    wa::PipelineAttachController controller;
    wa::CallLogStore store;
    wa::pipeline_attach_ui::SegmentSelection selection;
    ASSERT_TRUE(controller.start(child.dwProcessId));
    const auto receiveUntil = [&](const auto& complete) {
        const auto deadline = GetTickCount64() + 10000;
        do {
            for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
            if (complete()) return true;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        return false;
    };
    ASSERT_TRUE(receiveUntil([&] { return store.activeAttach(child.dwProcessId) != 0; }));
    const auto old = selection.selected();
    const auto instance = store.segment(old)->instance;
    const auto oldToken = store.segment(old)->token;
    const auto startupToken = base + L"-startup";
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(wa::startup_probe::Shared), (startupToken + L"-map").c_str());
    HANDLE startupGate = CreateEventW(nullptr, TRUE, FALSE, (startupToken + L"-gate").c_str());
    HANDLE startupEntered = CreateEventW(nullptr, TRUE, FALSE, (startupToken + L"-entered").c_str());
    ASSERT_TRUE(map && startupGate && startupEntered);
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wa::StartupRequest request;
    request.executable = (std::filesystem::path(self).parent_path() / L"StartupGateTarget.exe").wstring();
    request.arguments = L"--token=" + startupToken + L" --remote-gate --hold-before-entry";
    ASSERT_TRUE(controller.startStartup(request));
    ASSERT_EQ(WaitForSingleObject(startupEntered, 5000), WAIT_OBJECT_0);
    const auto starting = controller.startupStatus();
    ASSERT_TRUE(starting.running);
    HANDLE startingProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, starting.outcome.pid);
    ASSERT_NE(startingProcess, nullptr);
    const auto sibling = spawnSibling();
    wa::ProcessInstance siblingInstance;
    ASSERT_TRUE(wa::queryProcessInstance(sibling.dwProcessId, siblingInstance));
    ASSERT_TRUE(controller.startBatch({{siblingInstance, {}, wa::PipelineFlow::Render, 0}}));
    ASSERT_TRUE(controller.configure(instance, oldToken, 0));
    ASSERT_TRUE(receiveUntil([&] {
        return store.activeAttach(sibling.dwProcessId) && store.segment(old)->methods == 0;
    }));
    EXPECT_TRUE(controller.startupStatus().running);
    EXPECT_EQ(selection.selected(), old);
    ASSERT_TRUE(controller.stop(child.dwProcessId));
    ASSERT_TRUE(receiveUntil([&] { return store.segment(old)->state == wa::CallLogStore::SegmentState::Detached; }));
    EXPECT_TRUE(controller.startupStatus().running);
    EXPECT_TRUE(controller.attached(sibling.dwProcessId));
    ASSERT_TRUE(controller.cancelStartup());
    ASSERT_TRUE(receiveUntil([&] { return !controller.startupStatus().running; }));
    EXPECT_TRUE(controller.startupStatus().outcome.targetExitConfirmed);
    EXPECT_EQ(WaitForSingleObject(startingProcess, 0), WAIT_OBJECT_0);
    EXPECT_TRUE(controller.attached(sibling.dwProcessId));
    EXPECT_EQ(store.activeAttach(starting.outcome.pid), 0u);
    controller.shutdown();
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
    CloseHandle(startingProcess);
    CloseHandle(map);
    CloseHandle(startupGate);
    CloseHandle(startupEntered);
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

TEST_F(RemoteInjectionTest, AResidentHookWithARuntimeInstallationFaultRequiresRestart) {
    wa::OnDemandAttach first;
    ASSERT_TRUE(first.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    wchar_t name[64] = {};
    wa::hook_ipc::mapName(child.dwProcessId, name, 64);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    ASSERT_NE(map, nullptr);
    auto* ring = static_cast<wa::hook_ipc::Ring*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    ASSERT_NE(ring, nullptr);
    EXPECT_EQ(ring->installed, wa::hook_ipc::kInstallOk);
    // Model a later hook installation reporting an unrecoverable protection
    // fault. The installer regression separately proves this diagnostic source.
    InterlockedOr(reinterpret_cast<volatile LONG*>(&ring->coverageFlags),
                  wa::hook_ipc::kCoverageInstallationFault);
    EXPECT_TRUE(first.coverage().restartRequired);
    first.stop();
    wa::OnDemandAttach retry;
    const auto result = retry.start(child.dwProcessId, {}, wa::PipelineFlow::Render);
    EXPECT_FALSE(result);
    EXPECT_FALSE(retry.attached());
    EXPECT_NE(result.message.find("restart"), std::string::npos);
    EXPECT_EQ(ring->captureEnabled, 0u);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&ring->installed), wa::hook_ipc::kInstallFailed);
    const auto failedInstall = retry.start(child.dwProcessId, {}, wa::PipelineFlow::Render);
    EXPECT_FALSE(failedInstall);
    EXPECT_NE(failedInstall.message.find("restart"), std::string::npos);
    UnmapViewOfFile(ring);
    CloseHandle(map);
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

template<class Predicate>
bool waitUntil(Predicate predicate, DWORD timeout = 5000) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    do {
        if (predicate()) return true;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return predicate();
}

std::vector<wa::PipelineAttachController::Event> collectEvents(wa::PipelineAttachController& controller) {
    std::vector<wa::PipelineAttachController::Event> out;
    for (auto& batch : controller.poll())
        out.insert(out.end(), batch.events.begin(), batch.events.end());
    return out;
}

TEST_F(RemoteInjectionTest, BackgroundAttachCommitsThenDetachesWithoutEndingTarget) {
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    const auto attached = collectEvents(controller);
    ASSERT_EQ(attached.size(), 1u);
    EXPECT_EQ(attached[0].kind, wa::PipelineAttachController::EventKind::Attached);
    EXPECT_EQ(attached[0].pid, child.dwProcessId);
    EXPECT_TRUE(controller.attached(child.dwProcessId));
    EXPECT_FALSE(controller.start(child.dwProcessId));
    EXPECT_TRUE(controller.stop(child.dwProcessId));
    EXPECT_TRUE(controller.stop(child.dwProcessId));
    ASSERT_TRUE(waitUntil([&] { return !controller.anyAttached(); }));
    const auto ended = collectEvents(controller);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_EQ(ended[0].kind, wa::PipelineAttachController::EventKind::Detached);
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
}

TEST_F(RemoteInjectionTest, BatchCommitsEachTargetRejectsStaleIdentityAndSkipsActiveInstance) {
    wa::PipelineAttachController controller;
    wa::ProcessInstance instance, self;
    ASSERT_TRUE(wa::queryProcessInstance(child.dwProcessId, instance));
    ASSERT_TRUE(wa::queryProcessInstance(GetCurrentProcessId(), self));
    const auto mask = wa::methodBit(wa::MonitorMethod::Start);
    const wa::PipelineAttachController::Target target{instance, {}, wa::PipelineFlow::Render, mask};
    auto stale = target;
    ++stale.instance.created;
    ASSERT_TRUE(controller.startBatch({{self}, stale, target, target}));
    ASSERT_TRUE(waitUntil([&] { return !controller.batchStatus().running; }));
    const auto status = controller.batchStatus();
    ASSERT_EQ(status.items.size(), 3u);
    using State = wa::PipelineAttachController::ItemState;
    EXPECT_EQ(status.items[0].state, State::Rejected);
    EXPECT_EQ(status.items[1].state, State::TargetChanged);
    EXPECT_EQ(status.items[2].state, State::Succeeded);
    EXPECT_FALSE(status.hookPath.empty());
    EXPECT_TRUE(controller.attached(instance.pid));
    const auto events = collectEvents(controller);
    ASSERT_EQ(events.size(), 3u);
    const auto token = events.back().token;
    EXPECT_NE(token, 0u);
    ASSERT_TRUE(controller.startBatch({{instance, {}, wa::PipelineFlow::Render, 0}}));
    ASSERT_TRUE(waitUntil([&] { return !controller.batchStatus().running; }));
    ASSERT_EQ(controller.batchStatus().items.size(), 1u);
    EXPECT_EQ(controller.batchStatus().items[0].state, State::Skipped);
    EXPECT_EQ(controller.lastMethods(instance), mask);
    EXPECT_TRUE(collectEvents(controller).empty());
    EXPECT_TRUE(controller.stopAll());
    ASSERT_TRUE(waitUntil([&] { return !controller.anyAttached(); }));
    EXPECT_EQ(collectEvents(controller).front().token, token);
}

TEST_F(RemoteInjectionTest, MethodSelectionPublishesWithoutAnApiAndRetainsOnlySuccessfulInstanceState) {
    wa::ProcessInstance instance;
    ASSERT_TRUE(wa::queryProcessInstance(child.dwProcessId, instance));
    const uint32_t initial = wa::methodBit(wa::MonitorMethod::Start);
    const uint32_t updated = wa::methodBit(wa::MonitorMethod::CaptureGetBuffer) |
        wa::methodBit(wa::MonitorMethod::GetChannelCount) | wa::methodBit(wa::MonitorMethod::GetAudioEffects);
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(instance.pid, {}, wa::PipelineFlow::Render, instance, initial));
    ASSERT_TRUE(waitUntil([&] { return !controller.snapshot().pendingPid; }));
    const auto started = collectEvents(controller);
    ASSERT_EQ(started.size(), 1u);
    ASSERT_EQ(started[0].kind, wa::PipelineAttachController::EventKind::Attached);
    const uint64_t token = started[0].token;
    EXPECT_EQ(started[0].methods, initial);
    struct Mapping {
        HANDLE handle = nullptr;
        wa::hook_ipc::Ring* ring = nullptr;
        ~Mapping() { if (ring) UnmapViewOfFile(ring); if (handle) CloseHandle(handle); }
    } monitorView;
    wchar_t name[64]{};
    wa::hook_ipc::mapName(instance.pid, name, 64);
    monitorView.handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    ASSERT_NE(monitorView.handle, nullptr);
    monitorView.ring = static_cast<wa::hook_ipc::Ring*>(MapViewOfFile(monitorView.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    ASSERT_NE(monitorView.ring, nullptr);
    const auto patches = monitorView.ring->patchAccepted;
    const auto generation = monitorView.ring->observationGeneration;
    EXPECT_EQ(static_cast<uint32_t>(wa::hook_ipc::monitorState(*monitorView.ring)), initial);
    EXPECT_EQ(monitorView.ring->writeIndex + monitorView.ring->pumpWriteIndex, 0u);
    std::vector<wa::PipelineAttachController::Event> events;
    const auto receive = [&] {
        events = collectEvents(controller);
        return !events.empty();
    };
    ASSERT_TRUE(controller.configure(instance, token, updated));
    ASSERT_TRUE(waitUntil(receive));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, wa::PipelineAttachController::EventKind::Configured);
    EXPECT_EQ(monitorView.ring->writeIndex + monitorView.ring->pumpWriteIndex, 0u);
    EXPECT_EQ(monitorView.ring->patchAccepted, patches);
    EXPECT_EQ(monitorView.ring->observationGeneration, generation);
    EXPECT_EQ(static_cast<uint32_t>(wa::hook_ipc::monitorState(*monitorView.ring)), updated);
    EXPECT_EQ(controller.lastMethods(instance), updated);
    auto reused = instance; ++reused.created;
    EXPECT_EQ(controller.lastMethods(reused), wa::kDefaultMonitorMethods);
    ASSERT_TRUE(controller.configure(reused, token, 0));
    ASSERT_TRUE(waitUntil(receive));
    EXPECT_EQ(events[0].kind, wa::PipelineAttachController::EventKind::ConfigurationFailed);
    EXPECT_EQ(controller.lastMethods(instance), updated);
    EXPECT_EQ(controller.lastMethods(reused), wa::kDefaultMonitorMethods);
    const uint32_t magic = monitorView.ring->magic;
    monitorView.ring->magic = 0;
    ASSERT_TRUE(controller.configure(instance, token, 0));
    ASSERT_TRUE(waitUntil(receive));
    EXPECT_EQ(events[0].kind, wa::PipelineAttachController::EventKind::ConfigurationFailed);
    EXPECT_NE(events[0].result.message.find("restart"), std::string::npos);
    EXPECT_EQ(controller.lastMethods(instance), updated);
    EXPECT_EQ(static_cast<uint32_t>(wa::hook_ipc::monitorState(*monitorView.ring)), updated);
    monitorView.ring->magic = magic;
    ASSERT_TRUE(controller.stop(instance.pid));
    ASSERT_TRUE(waitUntil([&] { return !controller.attached(instance.pid); }));
    collectEvents(controller);
    ASSERT_TRUE(controller.start(instance.pid, {}, wa::PipelineFlow::Render, instance));
    ASSERT_TRUE(waitUntil([&] { return !controller.snapshot().pendingPid; }));
    const auto reattached = collectEvents(controller);
    ASSERT_EQ(reattached.size(), 1u);
    ASSERT_EQ(reattached[0].kind, wa::PipelineAttachController::EventKind::Attached);
    EXPECT_NE(reattached[0].token, token);
    EXPECT_EQ(reattached[0].methods, updated);
    EXPECT_EQ(monitorView.ring->patchAccepted, patches);
    EXPECT_EQ(static_cast<uint32_t>(wa::hook_ipc::monitorState(*monitorView.ring)), updated);
    ASSERT_TRUE(controller.configure(instance, token, 0));
    ASSERT_TRUE(waitUntil(receive));
    EXPECT_EQ(events[0].kind, wa::PipelineAttachController::EventKind::ConfigurationFailed);
    EXPECT_EQ(controller.lastMethods(instance), updated);
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
}

TEST_F(RemoteInjectionTest, BackgroundTargetExitPublishesAnOwnedEndResult) {
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    ASSERT_TRUE(controller.attached(child.dwProcessId));
    collectEvents(controller);
    SetEvent(stop);
    ASSERT_EQ(WaitForSingleObject(child.hProcess, 5000), WAIT_OBJECT_0);
    ASSERT_TRUE(waitUntil([&] { return !controller.anyAttached(); }));
    const auto ended = collectEvents(controller);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_EQ(ended[0].pid, child.dwProcessId);
    EXPECT_EQ(ended[0].kind, wa::PipelineAttachController::EventKind::Detached);
}

TEST_F(RemoteInjectionTest, ClearPublishesANewGenerationPreservesMethodsAndRejectsQueuedSourceData) {
    wa::PipelineAttachController controller;
    wa::CallLogStore store;
    wa::pipeline_attach_ui::SegmentSelection selection;
    const uint32_t methods = wa::methodBit(wa::MonitorMethod::Start) |
        wa::methodBit(wa::MonitorMethod::GetChannelCount);
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render, {}, methods));
    ASSERT_TRUE(waitUntil([&] { return !controller.snapshot().pendingPid; }));
    for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
    const auto id = selection.selected();
    ASSERT_NE(id, 0u);
    const auto instance = store.segment(id)->instance;
    const auto token = store.segment(id)->token;
    struct Mapping {
        HANDLE handle = nullptr;
        wa::hook_ipc::Ring* ring = nullptr;
        ~Mapping() { if (ring) UnmapViewOfFile(ring); if (handle) CloseHandle(handle); }
    } mappingView;
    wchar_t name[64]{};
    wa::hook_ipc::mapName(instance.pid, name, 64);
    mappingView.handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    ASSERT_NE(mappingView.handle, nullptr);
    mappingView.ring = static_cast<wa::hook_ipc::Ring*>(MapViewOfFile(mappingView.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    ASSERT_NE(mappingView.ring, nullptr);
    auto& ring = *mappingView.ring;
    const auto generation = static_cast<uint32_t>(wa::hook_ipc::monitorState(ring) >> 32);
    wa::hook_ipc::CallPod pod{};
    pod.observationGeneration = generation;
    strcpy_s(pod.iface, "IAudioClient");
    strcpy_s(pod.method, "Start");
    ASSERT_TRUE(wa::hook_ipc::writeCall(ring, pod));
    wa::hook_ipc::countMethodCompletion(ring, wa::MonitorMethod::Start, generation, S_OK, false);
    auto pump = pod;
    pump.pump = 1;
    strcpy_s(pump.iface, "IAudioStreamVolume");
    strcpy_s(pump.method, "GetChannelCount");
    ASSERT_TRUE(wa::hook_ipc::writeCall(ring, pump));
    wa::hook_ipc::countMethodCompletion(ring, wa::MonitorMethod::GetChannelCount, generation, S_OK, false);
    ASSERT_TRUE(waitUntil([&] {
        for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
        return store.segment(id)->count == 1 && store.segment(id)->windowReceived == 1;
    }));
    const auto oldRecord = store.entries().front();
    const auto frozen = store.freezeExport();
    // Leave old data in both the shared ring and the controller result queue.
    ASSERT_TRUE(wa::hook_ipc::writeCall(ring, pod));
    wa::hook_ipc::countMethodCompletion(ring, wa::MonitorMethod::Start, generation, S_OK, false);
    ASSERT_TRUE(wa::hook_ipc::writeCall(ring, pump));
    wa::hook_ipc::countMethodCompletion(ring, wa::MonitorMethod::GetChannelCount, generation, S_OK, false);
    ASSERT_TRUE(controller.clear(instance, token));
    bool cleared = false;
    ASSERT_TRUE(waitUntil([&] {
        for (auto& batch : controller.poll()) {
            for (const auto& event : batch.events)
                if (event.kind == wa::PipelineAttachController::EventKind::Cleared) cleared = true;
            selection.apply(store, std::move(batch));
        }
        return cleared;
    }));
    EXPECT_EQ(static_cast<uint32_t>(wa::hook_ipc::monitorState(ring)), methods);
    EXPECT_EQ(static_cast<uint32_t>(wa::hook_ipc::monitorState(ring) >> 32), generation + 1);
    EXPECT_EQ(store.segment(id)->count, 0u);
    EXPECT_EQ(store.segment(id)->windowRetained, 0u);
    EXPECT_EQ(store.segment(id)->windowReceived, 0u);
    EXPECT_EQ(store.segment(id)->windowEvicted, 0u);
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[static_cast<size_t>(wa::MonitorMethod::GetChannelCount)].completed, 0u);
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[static_cast<size_t>(wa::MonitorMethod::Start)].completed, 0u);
    EXPECT_EQ(controller.lastMethods(instance), methods);
    EXPECT_EQ(selection.selected(), id);
    EXPECT_EQ(store.find(oldRecord->id), nullptr);
    EXPECT_EQ(frozen.records.front().get(), oldRecord.get());
    // Old completions after publication cannot increment this generation.
    wa::hook_ipc::countMethodCompletion(ring, wa::MonitorMethod::Start, generation, E_FAIL, false);
    pod.observationGeneration = generation + 1;
    ASSERT_TRUE(wa::hook_ipc::writeCall(ring, pod));
    wa::hook_ipc::countMethodCompletion(ring, wa::MonitorMethod::Start, generation + 1, S_OK, false);
    pump.observationGeneration = generation + 1;
    ASSERT_TRUE(wa::hook_ipc::writeCall(ring, pump));
    wa::hook_ipc::countMethodCompletion(ring, wa::MonitorMethod::GetChannelCount, generation + 1, E_FAIL, false);
    ASSERT_TRUE(waitUntil([&] {
        for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
        return store.segment(id)->count == 1 && store.segment(id)->windowReceived == 1 &&
            store.segment(id)->transport.statistics.methods[static_cast<size_t>(wa::MonitorMethod::Start)].completed == 1;
    }));
    const auto kept = store.entries().front();
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[static_cast<size_t>(wa::MonitorMethod::GetChannelCount)].failed, 1u);
    const auto start = store.segment(id)->statisticsStartedMs;
    const auto magic = ring.magic;
    ring.magic = 0;
    ASSERT_TRUE(controller.clear(instance, token));
    bool failed = false;
    ASSERT_TRUE(waitUntil([&] {
        for (auto& batch : controller.poll()) {
            for (const auto& event : batch.events)
                if (event.kind == wa::PipelineAttachController::EventKind::ClearFailed) {
                    failed = true;
                    EXPECT_FALSE(event.result);
                    EXPECT_NE(event.result.message.find("restart"), std::string::npos);
                }
            selection.apply(store, std::move(batch));
        }
        return failed;
    }));
    EXPECT_EQ(store.segment(id)->count, 1u);
    EXPECT_EQ(store.segment(id)->statisticsStartedMs, start);
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[static_cast<size_t>(wa::MonitorMethod::Start)].completed, 1u);
    EXPECT_EQ(store.find(kept->id), kept.get());
    EXPECT_EQ(controller.lastMethods(instance), methods);
    ring.magic = magic;
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
}

TEST_F(RemoteInjectionTest, ExportConsumesAlreadyQueuedControllerBatchOutsidePresentationCadence) {
    wa::PipelineAttachController controller;
    wa::CallLogStore store;
    wa::pipeline_attach_ui::SegmentSelection selection;
    wa::pipeline_attach_ui::PresentationRefresh presentation;
    const auto initial = wa::methodBit(wa::MonitorMethod::Start) | wa::methodBit(wa::MonitorMethod::GetChannelCount);
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render, {}, initial));
    ASSERT_TRUE(waitUntil([&] { return !controller.snapshot().pendingPid; }));
    for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
    const auto id = selection.selected();
    ASSERT_NE(id, 0u);
    const auto instance = store.segment(id)->instance;
    const auto token = store.segment(id)->token;
    const auto generation = store.segment(id)->observationGeneration;
    const auto other = store.registerAttach({9, 200}, "background", 999, 1);
    struct Mapping {
        HANDLE handle = nullptr;
        wa::hook_ipc::Ring* ring = nullptr;
        ~Mapping() { if (ring) UnmapViewOfFile(ring); if (handle) CloseHandle(handle); }
    } mappingView;
    wchar_t name[64]{};
    wa::hook_ipc::mapName(instance.pid, name, 64);
    mappingView.handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    ASSERT_NE(mappingView.handle, nullptr);
    mappingView.ring = static_cast<wa::hook_ipc::Ring*>(MapViewOfFile(mappingView.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    ASSERT_NE(mappingView.ring, nullptr);
    auto& ring = *mappingView.ring;
    wa::hook_ipc::CallPod call{};
    call.observationGeneration = generation;
    strcpy_s(call.iface, "IAudioClient");
    strcpy_s(call.method, "Start");
    ASSERT_TRUE(wa::hook_ipc::writeCall(ring, call));
    call.pump = 1;
    strcpy_s(call.iface, "IAudioStreamVolume");
    strcpy_s(call.method, "GetChannelCount");
    ASSERT_TRUE(wa::hook_ipc::writeCall(ring, call));
    wa::hook_ipc::countMethodCompletion(ring, wa::MonitorMethod::GetChannelCount, generation, S_OK, false);
    const auto configured = initial | wa::methodBit(wa::MonitorMethod::GetChannelVolume);
    ASSERT_TRUE(controller.configure(instance, token, configured));
    ASSERT_TRUE(controller.stop(instance.pid));
    // The receiver has committed the tail and end event to its owned queue.
    // Do not poll it into Store before exercising the production export seam.
    ASSERT_TRUE(waitUntil([&] { return !controller.anyAttached(); }));
    EXPECT_EQ(store.segment(id)->count, 0u);
    EXPECT_EQ(store.segment(id)->windowReceived, 0u);
    EXPECT_EQ(store.segment(id)->state, wa::CallLogStore::SegmentState::Monitoring);
    EXPECT_TRUE(presentation.take(1000));
    EXPECT_FALSE(presentation.take(1050));
    auto snapshot = selection.freezeExport(store, 2000, [&] {
        for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
    });
    ASSERT_TRUE(snapshot.segment);
    EXPECT_EQ(selection.selected(), id);
    EXPECT_NE(selection.selected(), other);
    EXPECT_FALSE(presentation.take(1050));
    EXPECT_TRUE(presentation.take(1100));
    EXPECT_EQ(snapshot.segment->state, wa::CallLogStore::SegmentState::Detached);
    EXPECT_EQ(snapshot.segment->methods, configured);
    EXPECT_EQ(snapshot.segment->transport.statistics.methods[static_cast<size_t>(wa::MonitorMethod::GetChannelCount)].completed, 1u);
    ASSERT_EQ(snapshot.records.size(), 2u);
    EXPECT_EQ(snapshot.records[0]->call.method, "Start");
    EXPECT_EQ(snapshot.records[1]->call.method, "GetChannelCount");
    const auto path = std::filesystem::temp_directory_path() / L"winaudio-received-export.jsonl";
    std::filesystem::remove(path);
    wa::CallLogExportJob job;
    ASSERT_TRUE(job.start(std::move(snapshot), path.wstring()));
    wa::CallLogExportResult result;
    ASSERT_TRUE(job.wait(result));
    ASSERT_TRUE(result.result);
    EXPECT_EQ(result.recordsWritten, 2u);
    std::ifstream file(path, std::ios::binary);
    const std::string content(std::istreambuf_iterator<char>(file), {});
    EXPECT_NE(content.find("\"state\":\"detached\""), std::string::npos);
    EXPECT_NE(content.find("\"statistics.IAudioStreamVolume.GetChannelCount.completed\":1"), std::string::npos);
    EXPECT_NE(content.find("\"method\":\"Start\""), std::string::npos);
    EXPECT_NE(content.find("\"method\":\"GetChannelCount\""), std::string::npos);
}

TEST_F(RemoteInjectionTest, FrozenExpectedIdentityRejectsPidReuseBeforeInjection) {
    wa::ProcessInstance expected;
    ASSERT_TRUE(wa::queryProcessInstance(child.dwProcessId, expected));
    ++expected.created;  // Deterministic replacement identity, not PID reuse luck.
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render, expected));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    const auto events = collectEvents(controller);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, wa::PipelineAttachController::EventKind::Failed);
    EXPECT_EQ(events[0].token, 0u);
    EXPECT_EQ(events[0].result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED)));
    EXPECT_FALSE(controller.anyAttached());
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
}

TEST_F(RemoteInjectionTest, ClearFailsClosedAtGenerationExhaustionWithoutWrappingOrResettingCounts) {
    wa::OnDemandAttach attach;
    ASSERT_TRUE(attach.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    const auto instance = attach.instance();
    wchar_t name[64]{};
    wa::hook_ipc::mapName(instance.pid, name, 64);
    struct Mapping {
        HANDLE handle = nullptr;
        wa::hook_ipc::Ring* ring = nullptr;
        ~Mapping() { if (ring) UnmapViewOfFile(ring); if (handle) CloseHandle(handle); }
    } view;
    view.handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    ASSERT_NE(view.handle, nullptr);
    view.ring = static_cast<wa::hook_ipc::Ring*>(MapViewOfFile(view.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    ASSERT_NE(view.ring, nullptr);
    attach.stop();
    wa::hook_ipc::publishMonitorState(*view.ring, wa::kDefaultMonitorMethods, UINT32_MAX - 1);
    ASSERT_TRUE(attach.start(instance.pid, {}, wa::PipelineFlow::Render, nullptr, instance));
    wa::hook_ipc::countMethodCompletion(*view.ring, wa::MonitorMethod::Start, UINT32_MAX, S_OK, false);
    const auto before = wa::hook_ipc::monitorState(*view.ring);
    const auto result = attach.clear(instance);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW)));
    EXPECT_EQ(wa::hook_ipc::monitorState(*view.ring), before);
    EXPECT_EQ(attach.transportStatus().statistics.methods[static_cast<size_t>(wa::MonitorMethod::Start)].completed, 1u);
    auto reused = instance;
    ++reused.created;
    EXPECT_FALSE(attach.clear(reused));
    EXPECT_EQ(wa::hook_ipc::monitorState(*view.ring), before);
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
}

TEST_F(RemoteInjectionTest, ExitedFrozenRequestNeverBecomesSuccessfulSegment) {
    wa::ProcessInstance expected;
    ASSERT_TRUE(wa::queryProcessInstance(child.dwProcessId, expected));
    SetEvent(stop);
    ASSERT_EQ(WaitForSingleObject(child.hProcess, 5000), WAIT_OBJECT_0);
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render, expected));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    const auto events = collectEvents(controller);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, wa::PipelineAttachController::EventKind::Failed);
    EXPECT_EQ(events[0].token, 0u);
    EXPECT_FALSE(controller.anyAttached());
}

TEST_F(RemoteInjectionTest, BackgroundInstallationFailurePreservesAnExistingTarget) {
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    ASSERT_TRUE(controller.attached(child.dwProcessId));
    collectEvents(controller);
    ASSERT_TRUE(controller.start(GetCurrentProcessId()));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    const auto failed = collectEvents(controller);
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_EQ(failed[0].kind, wa::PipelineAttachController::EventKind::Failed);
    EXPECT_FALSE(failed[0].result);
    EXPECT_TRUE(controller.attached(child.dwProcessId));
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
}

TEST_F(RemoteInjectionTest, TransportQueueLossAndFinalSnapshotBelongToTheirSegment) {
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return !controller.snapshot().pendingPid; }));
    const auto started = collectEvents(controller);
    ASSERT_EQ(started.size(), 1u);
    ASSERT_EQ(started.front().kind, wa::PipelineAttachController::EventKind::Attached);
    const auto token = started.front().token;
    const auto instance = started.front().instance;
    wchar_t name[64]{};
    wa::hook_ipc::mapName(instance.pid, name, 64);
    struct Mapping {
        HANDLE handle = nullptr;
        wa::hook_ipc::Ring* ring = nullptr;
        ~Mapping() { if (ring) UnmapViewOfFile(ring); if (handle) CloseHandle(handle); }
    } ringView;
    ringView.handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    ASSERT_NE(ringView.handle, nullptr);
    ringView.ring = static_cast<wa::hook_ipc::Ring*>(MapViewOfFile(ringView.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    ASSERT_NE(ringView.ring, nullptr);
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().droppedBatches != 0; }));
    // Synthetic metadata in a real, independently running target ringView. The
    // real receiver/queue must attribute loss without needing an audio device.
    wa::hook_ipc::CallPod pod{};
    pod.observationGeneration = static_cast<uint32_t>(wa::hook_ipc::monitorState(*ringView.ring) >> 32);
    strcpy_s(pod.iface, "SyntheticTransport");
    strcpy_s(pod.method, "control");
    ASSERT_TRUE(wa::hook_ipc::writeCall(*ringView.ring, pod));
    pod.pump = 1;
    strcpy_s(pod.method, "pump");
    ASSERT_TRUE(wa::hook_ipc::writeCall(*ringView.ring, pod));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().droppedCalls == 1; }));
    // The batch capacity is finite even though the sources keep being drained.
    EXPECT_LE(controller.poll().size(), wa::PipelineAttachController::kQueueCapacity);
    wa::HookTransportStatus status;
    ASSERT_TRUE(waitUntil([&] {
        for (const auto& batch : controller.poll()) for (const auto& update : batch.transport) {
            EXPECT_EQ(update.instance, instance);
            EXPECT_EQ(update.token, token);
            EXPECT_GT(update.status.snapshotSequence, status.snapshotSequence);
            status = update.status;
        }
        return status.controllerDroppedControl == 1 && status.controllerDroppedPump == 1;
    }));
    EXPECT_EQ(status.control.overwritten + status.control.writeRejected, 0u);
    EXPECT_EQ(status.pump.overwritten + status.pump.writeRejected, 0u);
    EXPECT_FALSE(status.control.unknown);
    EXPECT_GT(status.snapshotMs, 0);
    ASSERT_TRUE(controller.stop(instance.pid));
    ASSERT_TRUE(waitUntil([&] { return !controller.anyAttached(); }));
    bool ended = false;
    for (const auto& batch : controller.poll()) {
        for (const auto& update : batch.transport) if (update.token == token) {
            EXPECT_GT(update.status.snapshotSequence, status.snapshotSequence);
            status = update.status;
        }
        for (const auto& event : batch.events) if (event.kind == wa::PipelineAttachController::EventKind::Detached) {
            ended = true;
            EXPECT_EQ(event.snapshotMs, status.snapshotMs);
        }
    }
    EXPECT_TRUE(ended);
    EXPECT_TRUE(status.control.unknown);
    EXPECT_TRUE(status.pump.unknown);
    EXPECT_EQ(status.controllerDroppedControl, 1u);
    EXPECT_EQ(status.controllerDroppedPump, 1u);
}

TEST_F(RemoteInjectionTest, BackgroundQueuesStayBoundedAndReportOverload) {
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    ASSERT_TRUE(controller.attached(child.dwProcessId));
    // The GUI deliberately stops consuming. Transport snapshots still arrive
    // when this hardware-independent target has made no audio calls.
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().droppedBatches != 0; }));
    for (size_t i = 0; i < wa::PipelineAttachController::kQueueCapacity; ++i)
        ASSERT_TRUE(controller.stop(child.dwProcessId));
    EXPECT_FALSE(controller.stop(child.dwProcessId));
    EXPECT_EQ(controller.snapshot().rejectedCommands, 1u);
    const auto batches = controller.poll();
    EXPECT_LE(batches.size(), wa::PipelineAttachController::kQueueCapacity);
    ASSERT_TRUE(waitUntil([&] { return !controller.anyAttached(); }));
    const auto ended = collectEvents(controller);
    ASSERT_EQ(ended.size(), 1u);
    EXPECT_EQ(ended[0].kind, wa::PipelineAttachController::EventKind::Detached);
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
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

TEST_F(AttachLifecycleTest, ExistingTargetDetachesWhileSecondInstallationIsStillGated) {
    struct Sibling {
        PROCESS_INFORMATION process{};
        HANDLE gate, stop;
        ~Sibling() {
            SetEvent(gate);
            SetEvent(stop);
            if (process.hProcess) {
                EXPECT_EQ(WaitForSingleObject(process.hProcess, 5000), WAIT_OBJECT_0);
                CloseHandle(process.hProcess);
                CloseHandle(process.hThread);
            }
        }
    } sibling{{}, gate, stop};
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto exe = std::filesystem::path(self).parent_path() / L"RemoteInjectionTarget.exe";
    std::wstring command = L"\"" + exe.wstring() + L"\" \"" + base + L"\"";
    ResetEvent(ready);
    STARTUPINFOW startup{sizeof(startup)};
    ASSERT_TRUE(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &startup, &sibling.process));
    ASSERT_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);
    wa::PipelineAttachController controller;
    ASSERT_TRUE(controller.start(sibling.process.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    ASSERT_TRUE(controller.attached(sibling.process.dwProcessId));
    const auto attachedEvents = collectEvents(controller);
    ASSERT_EQ(attachedEvents.size(), 1u);
    const auto siblingToken = attachedEvents.front().token;
    const auto threads = threadCount(child.dwProcessId);
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return threadCount(child.dwProcessId) > threads; }));
    size_t received = 0;
    ASSERT_TRUE(waitUntil([&] {
        for (const auto& batch : controller.poll())
            for (const auto& item : batch.transport)
                if (item.token == siblingToken) ++received;
        return received >= 3;
    }));
    EXPECT_EQ(controller.snapshot().pendingPid, child.dwProcessId);
    EXPECT_EQ(WaitForSingleObject(gate, 0), WAIT_TIMEOUT);
    ASSERT_TRUE(controller.stop(sibling.process.dwProcessId));
    ASSERT_TRUE(waitUntil([&] { return !controller.attached(sibling.process.dwProcessId); }));
    const auto detached = collectEvents(controller);
    ASSERT_EQ(detached.size(), 1u);
    EXPECT_EQ(detached[0].kind, wa::PipelineAttachController::EventKind::Detached);
    EXPECT_EQ(detached[0].pid, sibling.process.dwProcessId);
    EXPECT_EQ(controller.snapshot().pendingPid, child.dwProcessId);
    EXPECT_EQ(WaitForSingleObject(gate, 0), WAIT_TIMEOUT);
    EXPECT_EQ(WaitForSingleObject(sibling.process.hProcess, 0), WAIT_TIMEOUT);
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
    EXPECT_TRUE(controller.stop(child.dwProcessId));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    SetEvent(gate);
    EXPECT_TRUE(waitUntil([&] { return controller.snapshot().cleanupPending == 0; }));
}

TEST_F(AttachLifecycleTest, CancellationRetainsRealInjectionAndPreventsLateCapture) {
    exercisePending(true);
}

TEST_F(AttachLifecycleTest, BatchTimeoutPreservesOldReceptionConfigurationAndDetachThenContinuesMixedQueue) {
    HANDLE exitGo = CreateEventW(nullptr, TRUE, FALSE, (base + L"-exit").c_str());
    ASSERT_NE(exitGo, nullptr);
    struct Close { HANDLE value; ~Close() { SetEvent(value); CloseHandle(value); } } close{exitGo};
    const auto old = spawnSibling();
    const auto exiting = spawnSibling(true);
    const auto success = spawnSibling();
    wa::ProcessInstance oldInstance, gatedInstance, exitInstance, self, successInstance;
    ASSERT_TRUE(wa::queryProcessInstance(old.dwProcessId, oldInstance));
    ASSERT_TRUE(wa::queryProcessInstance(child.dwProcessId, gatedInstance));
    ASSERT_TRUE(wa::queryProcessInstance(exiting.dwProcessId, exitInstance));
    ASSERT_TRUE(wa::queryProcessInstance(GetCurrentProcessId(), self));
    ASSERT_TRUE(wa::queryProcessInstance(success.dwProcessId, successInstance));
    wa::PipelineAttachController controller;
    wa::CallLogStore store;
    wa::pipeline_attach_ui::SegmentSelection selection;
    const auto initial = wa::methodBit(wa::MonitorMethod::Start);
    ASSERT_TRUE(controller.start(old.dwProcessId, {}, wa::PipelineFlow::Render, oldInstance, initial));
    ASSERT_TRUE(waitUntil([&] { return !controller.snapshot().pendingPid; }));
    for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
    const auto oldId = selection.selected();
    ASSERT_NE(oldId, 0u);
    const auto token = store.segment(oldId)->token;
    wchar_t name[64]{};
    wa::hook_ipc::mapName(old.dwProcessId, name, 64);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    ASSERT_NE(map, nullptr);
    auto* ring = static_cast<wa::hook_ipc::Ring*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    ASSERT_NE(ring, nullptr);
    struct View { HANDLE handle; wa::hook_ipc::Ring* ring; ~View() { UnmapViewOfFile(ring); CloseHandle(handle); } } view{map, ring};
    const auto threads = threadCount(child.dwProcessId);
    std::vector<wa::PipelineAttachController::Target> requests{{gatedInstance}, {exitInstance}, {self},
        {successInstance, {}, wa::PipelineFlow::Render, initial}};
    ASSERT_TRUE(controller.startBatch(requests));
    requests.back().methods = 0;
    requests.clear();
    ASSERT_TRUE(waitUntil([&] { return threadCount(child.dwProcessId) > threads; }));
    EXPECT_FALSE(controller.start(old.dwProcessId));
    EXPECT_FALSE(controller.startBatch({{self}}));
    SetEvent(exitGo);
    ASSERT_EQ(WaitForSingleObject(exiting.hProcess, 5000), WAIT_OBJECT_0);
    const auto consume = [&] {
        for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
    };
    for (uint32_t sequence = 1; sequence <= 3; ++sequence) {
        wa::hook_ipc::CallPod call{};
        call.observationGeneration = ring->observationGeneration;
        strcpy_s(call.iface, "IAudioClient"); strcpy_s(call.method, "Start");
        call.streamId = sequence;
        ASSERT_TRUE(wa::hook_ipc::writeCall(*ring, call));
        ASSERT_TRUE(waitUntil([&] { consume(); return store.segment(oldId)->count == sequence; }));
        EXPECT_EQ(WaitForSingleObject(gate, 0), WAIT_TIMEOUT);
    }
    ASSERT_TRUE(controller.configure(oldInstance, token, 0));
    ASSERT_TRUE(waitUntil([&] { consume(); return store.segment(oldId)->methods == 0; }));
    EXPECT_EQ(WaitForSingleObject(gate, 0), WAIT_TIMEOUT);
    ASSERT_TRUE(controller.stop(old.dwProcessId));
    ASSERT_TRUE(waitUntil([&] { consume(); return !controller.attached(old.dwProcessId); }));
    EXPECT_EQ(WaitForSingleObject(gate, 0), WAIT_TIMEOUT);
    ASSERT_TRUE(waitUntil([&] { consume(); return !controller.batchStatus().running; }, 16000));
    using State = wa::PipelineAttachController::ItemState;
    const auto result = controller.batchStatus();
    ASSERT_EQ(result.items.size(), 4u);
    EXPECT_EQ(result.items[0].state, State::CleanupPending);
    EXPECT_EQ(result.items[1].state, State::TargetChanged);
    EXPECT_EQ(result.items[2].state, State::Rejected);
    EXPECT_EQ(result.items[3].state, State::Succeeded);
    EXPECT_EQ(result.items[3].target.methods, initial); // Frozen before the dynamic update.
    EXPECT_FALSE(controller.attached(old.dwProcessId));
    EXPECT_TRUE(controller.attached(success.dwProcessId));
    EXPECT_EQ(controller.lastMethods(oldInstance), 0u);
    EXPECT_EQ(controller.lastMethods(successInstance), initial);
    EXPECT_EQ(selection.selected(), oldId);
    EXPECT_EQ(store.segment(oldId)->count, 3u);
    EXPECT_EQ(store.segment(oldId)->state, wa::CallLogStore::SegmentState::Detached);
    EXPECT_EQ(controller.snapshot().cleanupPending, 1u);
    SetEvent(gate);
    ASSERT_TRUE(waitUntil([&] { consume(); return !controller.snapshot().cleanupPending; }));
    EXPECT_FALSE(controller.attached(child.dwProcessId));
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
    std::printf("batch_wait_progress controls=3 config=1 detach=1 timeout=1 exited=1 rejected=1 success=1 old_segment=%llu\n", oldId);
}

TEST_F(AttachLifecycleTest, BatchDetachAllPreventsQueuedInjectionAndLateCommit) {
    const auto next = spawnSibling();
    wa::ProcessInstance gatedInstance, nextInstance;
    ASSERT_TRUE(wa::queryProcessInstance(child.dwProcessId, gatedInstance));
    ASSERT_TRUE(wa::queryProcessInstance(next.dwProcessId, nextInstance));
    wa::PipelineAttachController controller;
    const auto threads = threadCount(child.dwProcessId);
    ASSERT_TRUE(controller.startBatch({{gatedInstance}, {nextInstance}}));
    ASSERT_TRUE(waitUntil([&] { return threadCount(child.dwProcessId) > threads; }));
    EXPECT_EQ(controller.batchStatus().items[1].state, wa::PipelineAttachController::ItemState::Queued);
    ASSERT_TRUE(controller.stopAll());
    ASSERT_TRUE(waitUntil([&] { return !controller.batchStatus().running; }));
    EXPECT_FALSE(controller.anyAttached());
    const auto status = controller.batchStatus();
    ASSERT_EQ(status.items.size(), 2u);
    EXPECT_EQ(status.items[0].state, wa::PipelineAttachController::ItemState::Cancelled);
    EXPECT_EQ(status.items[1].state, wa::PipelineAttachController::ItemState::Cancelled);
    bool loaded = false;
    EXPECT_TRUE(wa::inspectHookModules(next.dwProcessId, status.hookPath, loaded));
    EXPECT_FALSE(loaded);
    EXPECT_EQ(controller.snapshot().cleanupPending, 1u);
    SetEvent(gate);
    ASSERT_TRUE(waitUntil([&] { collectEvents(controller); return !controller.snapshot().cleanupPending; }));
    EXPECT_FALSE(controller.anyAttached());
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
    EXPECT_EQ(WaitForSingleObject(next.hProcess, 0), WAIT_TIMEOUT);
    std::printf("batch_cancel queued_not_injected=1 late_not_committed=1 targets_running=2 cleanup=0\n");
}

TEST_F(AttachLifecycleTest, BatchCancelPreservesCommittedSuccessAndStopsRemainingTargets) {
    const auto first = spawnSibling();
    const auto queued = spawnSibling();
    wa::ProcessInstance firstInstance, gatedInstance, queuedInstance;
    ASSERT_TRUE(wa::queryProcessInstance(first.dwProcessId, firstInstance));
    ASSERT_TRUE(wa::queryProcessInstance(child.dwProcessId, gatedInstance));
    ASSERT_TRUE(wa::queryProcessInstance(queued.dwProcessId, queuedInstance));
    wa::PipelineAttachController controller;
    const auto threads = threadCount(child.dwProcessId);
    ASSERT_TRUE(controller.startBatch({{firstInstance, {}, wa::PipelineFlow::Render}, {gatedInstance}, {queuedInstance}}));
    ASSERT_TRUE(waitUntil([&] { collectEvents(controller); return threadCount(child.dwProcessId) > threads; }));
    ASSERT_TRUE(controller.attached(first.dwProcessId));
    EXPECT_EQ(controller.batchStatus().items[2].state, wa::PipelineAttachController::ItemState::Queued);
    ASSERT_TRUE(controller.cancelBatch());
    ASSERT_TRUE(waitUntil([&] { collectEvents(controller); return !controller.batchStatus().running; }));
    const auto status = controller.batchStatus();
    ASSERT_EQ(status.items.size(), 3u);
    EXPECT_EQ(status.items[0].state, wa::PipelineAttachController::ItemState::Succeeded);
    EXPECT_EQ(status.items[1].state, wa::PipelineAttachController::ItemState::Cancelled);
    EXPECT_EQ(status.items[2].state, wa::PipelineAttachController::ItemState::Cancelled);
    EXPECT_TRUE(controller.attached(first.dwProcessId));
    bool loaded = false;
    EXPECT_TRUE(wa::inspectHookModules(queued.dwProcessId, status.hookPath, loaded));
    EXPECT_FALSE(loaded);
    EXPECT_EQ(controller.snapshot().cleanupPending, 1u);
    // The in-flight instance is reserved until the remote loader completes.
    ASSERT_TRUE(controller.startBatch({{gatedInstance}}));
    ASSERT_TRUE(waitUntil([&] { collectEvents(controller); return !controller.batchStatus().running; }));
    EXPECT_EQ(controller.batchStatus().items[0].state, wa::PipelineAttachController::ItemState::Rejected);
    EXPECT_TRUE(controller.attached(first.dwProcessId));
    SetEvent(gate);
    ASSERT_TRUE(waitUntil([&] { collectEvents(controller); return !controller.snapshot().cleanupPending; }));
    EXPECT_TRUE(controller.attached(first.dwProcessId));
    EXPECT_FALSE(controller.attached(child.dwProcessId));
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
    EXPECT_EQ(WaitForSingleObject(queued.hProcess, 0), WAIT_TIMEOUT);
    std::printf("batch_cancel committed_success=1 queued_not_injected=1 pending_duplicate_rejected=1 late_not_committed=1 cleanup=0\n");
}

TEST_F(AttachLifecycleTest, BackgroundCommandReturnsWhileRealLoaderRemainsBlocked) {
    wa::PipelineAttachController controller;
    auto command = std::async(std::launch::async, [&] {
        return controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render);
    });
    EXPECT_EQ(command.wait_for(std::chrono::milliseconds(150)), std::future_status::ready);
    EXPECT_EQ(WaitForSingleObject(gate, 0), WAIT_TIMEOUT);
    SetEvent(gate);
    command.get();
}

TEST_F(AttachLifecycleTest, BackgroundRepeatedCancelPreventsLateCommitAndKeepsCleanupManaged) {
    wa::PipelineAttachController controller;
    const auto threads = threadCount(child.dwProcessId);
    ASSERT_TRUE(controller.start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return threadCount(child.dwProcessId) > threads; }));
    EXPECT_TRUE(controller.stop(child.dwProcessId));
    EXPECT_TRUE(controller.stop(child.dwProcessId));
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().pendingPid == 0; }));
    const auto events = collectEvents(controller);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].kind, wa::PipelineAttachController::EventKind::Failed);
    EXPECT_EQ(events[0].result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
    EXPECT_FALSE(controller.anyAttached());
    EXPECT_EQ(controller.snapshot().cleanupPending, 1u);
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
    SetEvent(gate);
    ASSERT_TRUE(waitUntil([&] { return controller.snapshot().cleanupPending == 0; }));
    EXPECT_TRUE(collectEvents(controller).empty());
    EXPECT_FALSE(controller.anyAttached());
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
}

TEST_F(AttachLifecycleTest, BackgroundToolCloseCancelsPendingInstallWithoutKillingTarget) {
    const auto threads = threadCount(child.dwProcessId);
    auto controller = std::make_unique<wa::PipelineAttachController>();
    ASSERT_TRUE(controller->start(child.dwProcessId, {}, wa::PipelineFlow::Render));
    ASSERT_TRUE(waitUntil([&] { return threadCount(child.dwProcessId) > threads; }));
    const auto before = GetTickCount64();
    controller->shutdown();
    controller->shutdown();
    EXPECT_FALSE(controller->start(child.dwProcessId));
    controller.reset();
    EXPECT_LT(GetTickCount64() - before, 1000u);
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
    EXPECT_EQ(wa::OnDemandAttach::reapPending(), 1u);
    SetEvent(gate);
    EXPECT_TRUE(waitUntil([] { return wa::OnDemandAttach::reapPending() == 0; }));
    EXPECT_EQ(WaitForSingleObject(child.hProcess, 0), WAIT_TIMEOUT);
}

TEST_F(AttachLifecycleTest, TimeoutRetainsRealInjectionAndPreventsLateCapture) {
    exercisePending(false);
}

TEST_F(AttachLifecycleTest, CancelledLoadThatLaterFailsIsReaped) {
    missingHookProbe(true);
}
}
