#include <gtest/gtest.h>
#include <windows.h>
#include <tlhelp32.h>
#include "OnDemandAttach.h"
#include "RemoteInjection.h"
#include "HookModuleIdentity.h"
#include "HookedCallPod.h"
#include "StartupGateHarness.h"
#include <future>
#include <thread>
#include <string>
#include <cwchar>

namespace {
bool rejectThreadCreation = false;
HANDLE debuggerFaultReady = nullptr;
HANDLE WINAPI createDebuggerThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack, LPTHREAD_START_ROUTINE entry,
                                   LPVOID parameter, DWORD flags, LPDWORD id) {
    if (rejectThreadCreation) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    return ::CreateThread(attributes, stack, entry, parameter, flags, id);
}
BOOL WINAPI waitDebuggerEvent(LPDEBUG_EVENT event, DWORD timeout) {
    const BOOL available = ::WaitForDebugEvent(event, timeout);
    const DWORD error = available ? ERROR_SUCCESS : GetLastError();
    // Abrupt control-thread failure at a Win32 wait boundary, only after the
    // independent observer proved the real remote installation is pending.
    if (debuggerFaultReady && WaitForSingleObject(debuggerFaultReady, 0) == WAIT_OBJECT_0) ExitThread(71);
    SetLastError(error);
    return available;
}
}

// Compile the production entry point against one deterministic Win32 boundary.
// WinAudioCore remains unchanged; its StartupLaunch object is not linked here.
#define CreateThread createDebuggerThread
#define WaitForDebugEvent waitDebuggerEvent
#include "../core/StartupLaunch.cpp"
#undef WaitForDebugEvent
#undef CreateThread

TEST(StartupLaunchBoundary, ThreadCreationFailurePreservesOriginalSystemError) {
    wa::StartupRequest request;
    request.executable = L"not-created.exe";
    rejectThreadCreation = true;
    const auto out = wa::launchAtEntry(request,
        [](uint32_t, const std::atomic_bool&) { return wa::Result::Ok(); });
    rejectThreadCreation = false;
    EXPECT_FALSE(out.result);
    EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)));
    EXPECT_EQ(out.result.message, "Startup: startup debugger unavailable");
    EXPECT_EQ(out.pid, 0u);
    EXPECT_FALSE(out.committed);
}

namespace {
struct PendingFacts {
    uint32_t pid = 0;
    uint32_t observed = 0;
    uintptr_t gateParameter = 0;
    uintptr_t hookParameter = 0;
    wchar_t gatePath[MAX_PATH]{};
    wchar_t hookPath[MAX_PATH]{};
};

std::wstring binaryFolder() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring folder = self;
    folder.resize(folder.find_last_of(L"\\/") + 1);
    return folder;
}

// RemoteInjection uses a dedicated VirtualAllocEx allocation, with the path
// at its base. Observe actual target pages instead of exposing test state in
// OnDemandAttach or substituting its remote operation.
uintptr_t findPathAllocation(HANDLE process, const wchar_t* path) {
    const SIZE_T bytes = (wcslen(path) + 1) * sizeof(wchar_t);
    uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION memory{};
    while (VirtualQueryEx(process, reinterpret_cast<const void*>(address), &memory, sizeof(memory))) {
        if (memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE && memory.Protect == PAGE_READWRITE &&
            memory.BaseAddress == memory.AllocationBase && memory.RegionSize >= bytes) {
            wchar_t value[MAX_PATH]{};
            SIZE_T copied = 0;
            if (ReadProcessMemory(process, memory.BaseAddress, value, bytes, &copied) && copied == bytes &&
                !memcmp(value, path, bytes)) return reinterpret_cast<uintptr_t>(memory.BaseAddress);
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (next <= address) break;
        address = next;
    }
    return 0;
}

bool readablePath(HANDLE process, uintptr_t address, const wchar_t* expected) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION memory{};
    wchar_t value[MAX_PATH]{};
    SIZE_T copied = 0;
    const SIZE_T bytes = (wcslen(expected) + 1) * sizeof(wchar_t);
    return VirtualQueryEx(process, reinterpret_cast<const void*>(address), &memory, sizeof(memory)) &&
        memory.State == MEM_COMMIT && ReadProcessMemory(process, reinterpret_cast<const void*>(address), value, bytes, &copied) &&
        copied == bytes && !memcmp(value, expected, bytes);
}

uint32_t targetThreadCount(uint32_t pid) {
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

class PendingScenario {
public:
    explicit PendingScenario(const std::wstring& name) : token(name) {
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(wa::startup_probe::Shared), (token + L"-map").c_str());
        factsMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(PendingFacts), (token + L"-pending-map").c_str());
        if (factsMap) facts = static_cast<PendingFacts*>(MapViewOfFile(factsMap, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        for (size_t i = 0; i < 5; ++i)
            events[i] = CreateEventW(nullptr, TRUE, FALSE, (token + suffixes[i]).c_str());
    }
    ~PendingScenario() {
        if (events[0]) SetEvent(events[0]);
        if (monitor.joinable()) monitor.join();
        attachment.stop();
        if (ring) UnmapViewOfFile(ring);
        if (hookMap) CloseHandle(hookMap);
        if (target) {
            if (WaitForSingleObject(target, 0) != WAIT_OBJECT_0) {
                TerminateProcess(target, 90);
                WaitForSingleObject(target, 5000);
            }
            CloseHandle(target);
        }
        if (facts) UnmapViewOfFile(facts);
        for (HANDLE h : events) if (h) CloseHandle(h);
        if (factsMap) CloseHandle(factsMap);
        if (map) CloseHandle(map);
    }
    bool valid() const {
        return map && facts && events[0] && events[1] && events[2] && events[3] && events[4];
    }
    wa::Result prepare(uint32_t pid, const std::atomic_bool& cancelled) {
        facts->pid = pid;
        target = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                             PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_TERMINATE, FALSE, pid);
        if (!target) return wa::Result::Fail(static_cast<long>(E_FAIL), "pending target OpenProcess");
        const std::wstring gate = binaryFolder() + L"RemoteInjectionGate.dll";
        std::wstring hook;
        auto result = wa::stageHookModule(binaryFolder() + L"WinAudioHook.dll", hook);
        if (!result) return result;
        wcscpy_s(facts->gatePath, gate.c_str());
        wcscpy_s(facts->hookPath, hook.c_str());
        result = loader.begin(target, reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")), gate.c_str(), (gate.size() + 1) * sizeof(wchar_t));
        if (!result) return result;
        facts->gateParameter = reinterpret_cast<uintptr_t>(loader.parameter());
        if (WaitForSingleObject(events[1], 5000) != WAIT_OBJECT_0)
            return wa::Result::Fail(static_cast<long>(E_FAIL), "real loader did not enter gate");
        const uint32_t baselineThreads = targetThreadCount(pid);
        monitor = std::thread([this, pid, baselineThreads] {
            wchar_t name[64]{};
            wa::hook_ipc::mapName(pid, name, 64);
            const ULONGLONG deadline = GetTickCount64() + 5000;
            while (GetTickCount64() < deadline && WaitForSingleObject(target, 0) == WAIT_TIMEOUT) {
                if (!hookMap) hookMap = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
                if (hookMap && !ring) ring = static_cast<const wa::hook_ipc::Ring*>(
                    MapViewOfFile(hookMap, FILE_MAP_READ, 0, 0, sizeof(wa::hook_ipc::Ring)));
                facts->hookParameter = findPathAllocation(target, facts->hookPath);
                if (ring && facts->hookParameter && ring->installed == wa::hook_ipc::kInstallPending &&
                    readablePath(target, facts->gateParameter, facts->gatePath) && targetThreadCount(pid) > baselineThreads) {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(&facts->observed), 1);
                    SetEvent(events[2]);
                    return;
                }
                WaitForSingleObject(events[0], 1);
            }
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&facts->observed), 2);
            SetEvent(events[2]);
        });
        result = attachment.start(pid, {}, wa::PipelineFlow::Render, &cancelled);
        monitor.join();
        return result;
    }
    bool activeResources() const {
        return facts->observed == 1 && target && WaitForSingleObject(target, 0) == WAIT_TIMEOUT &&
            loader.pending() && ring && ring->installed == wa::hook_ipc::kInstallPending &&
            readablePath(target, facts->gateParameter, facts->gatePath) &&
            readablePath(target, facts->hookParameter, facts->hookPath);
    }
    bool reclaimed() {
        if (!target || WaitForSingleObject(target, 0) != WAIT_OBJECT_0) return false;
        attachment.stop();
        const bool remoteDone = loader.poll() && !loader.pending() && !loader.parameter();
        const bool reservationsDone = wa::OnDemandAttach::reapPending() == 0;
        if (ring) { UnmapViewOfFile(ring); ring = nullptr; }
        if (hookMap) { CloseHandle(hookMap); hookMap = nullptr; }
        wchar_t name[64]{};
        wa::hook_ipc::mapName(facts->pid, name, 64);
        HANDLE remaining = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        const DWORD error = remaining ? ERROR_SUCCESS : GetLastError();
        if (remaining) CloseHandle(remaining);
        return remoteDone && reservationsDone && !remaining && error == ERROR_FILE_NOT_FOUND;
    }
    std::wstring token;
    HANDLE map = nullptr, factsMap = nullptr, target = nullptr, hookMap = nullptr;
    HANDLE events[5]{};
    static constexpr const wchar_t* suffixes[] = {L"-gate", L"-entered", L"-pending", L"-ready", L"-stop"};
    PendingFacts* facts = nullptr;
    const wa::hook_ipc::Ring* ring = nullptr;
    wa::RemoteInjection loader;
    wa::OnDemandAttach attachment;
    std::thread monitor;
};

wa::StartupRequest pendingRequest(const std::wstring& token, uint32_t timeout) {
    wa::StartupRequest request;
    request.executable = binaryFolder() + L"StartupGateTarget.exe";
    request.arguments = L"--token=" + token + L" --remote-gate";
    request.timeoutMs = timeout;
    return request;
}

TEST(StartupLaunchBoundary, PendingRealInstallationSurvivesUntilCancelledOrTimedOutTargetExit) {
    for (bool cancel : {true, false}) {
        const std::wstring token = L"Local\\WinAudio-StartupPending-" + std::to_wstring(GetCurrentProcessId()) +
                                  (cancel ? L"-cancel" : L"-timeout");
        PendingScenario scenario(token);
        ASSERT_TRUE(scenario.valid());
        std::atomic_bool cancelled{false};
        auto launched = std::async(std::launch::async, [&] {
            return wa::launchAtEntry(pendingRequest(token, cancel ? 10000 : 3500),
                [&](uint32_t pid, const std::atomic_bool& stop) { return scenario.prepare(pid, stop); }, &cancelled);
        });
        const DWORD reached = WaitForSingleObject(scenario.events[2], 7000);
        EXPECT_EQ(reached, WAIT_OBJECT_0);
        EXPECT_TRUE(scenario.activeResources());
        if (cancel) cancelled.store(true);
        const auto out = launched.get();
        EXPECT_FALSE(out.result);
        EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(cancel ? ERROR_CANCELLED : ERROR_TIMEOUT)));
        EXPECT_FALSE(out.committed);
        EXPECT_TRUE(out.targetExitConfirmed);
        EXPECT_TRUE(scenario.reclaimed());
        std::printf("pending_install mode=%s pid=%u observed=%u confirmed=%d committed=%d hr=0x%08lX reclaimed=%d\n",
                    cancel ? "cancel" : "timeout", out.pid, scenario.facts->observed, out.targetExitConfirmed,
                    out.committed, out.result.code, scenario.reclaimed());
    }
}

TEST(StartupLaunchBoundary, DebuggerThreadFailureDuringRealInstallationIsSupervisedAndJoined) {
    const std::wstring token = L"Local\\WinAudio-StartupPending-" + std::to_wstring(GetCurrentProcessId()) + L"-worker";
    PendingScenario scenario(token);
    ASSERT_TRUE(scenario.valid());
    debuggerFaultReady = scenario.events[2];
    const auto out = wa::launchAtEntry(pendingRequest(token, 10000),
        [&](uint32_t pid, const std::atomic_bool& stop) { return scenario.prepare(pid, stop); });
    debuggerFaultReady = nullptr;
    EXPECT_EQ(scenario.facts->observed, 1u);
    EXPECT_FALSE(out.result);
    EXPECT_EQ(out.result.code, static_cast<long>(HRESULT_FROM_WIN32(ERROR_UNHANDLED_EXCEPTION)));
    EXPECT_FALSE(out.committed);
    EXPECT_TRUE(out.targetExitConfirmed);
    EXPECT_TRUE(scenario.reclaimed());
    std::printf("pending_install mode=worker-exit pid=%u observed=%u confirmed=%d committed=%d reclaimed=%d\n",
                out.pid, scenario.facts->observed, out.targetExitConfirmed, out.committed, scenario.reclaimed());
}

int pendingController(const std::wstring& token) {
    PendingScenario scenario(token);
    if (!scenario.valid()) return 2;
    const auto out = wa::launchAtEntry(pendingRequest(token, 15000),
        [&](uint32_t pid, const std::atomic_bool& stop) { return scenario.prepare(pid, stop); });
    return out.result ? 3 : 4;  // Parent must kill this controller while pending.
}

TEST(StartupLaunchBoundary, ForcedControllerExitReclaimsRealPendingInstallationWithItsTarget) {
    const std::wstring token = L"Local\\WinAudio-StartupPending-" + std::to_wstring(GetCurrentProcessId()) + L"-controller";
    PendingScenario observation(token);
    ASSERT_TRUE(observation.valid());
    const std::wstring exe = binaryFolder() + L"StartupLaunchBoundaryTests.exe";
    std::wstring command = L"\"" + exe + L"\" --pending-controller " + token;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION controller{};
    ASSERT_TRUE(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &controller));
    const DWORD reached = WaitForSingleObject(observation.events[2], 10000);
    EXPECT_EQ(reached, WAIT_OBJECT_0);
    EXPECT_EQ(observation.facts->observed, 1u);
    HANDLE target = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                FALSE, observation.facts->pid);
    EXPECT_NE(target, nullptr);
    if (target) {
        EXPECT_EQ(WaitForSingleObject(target, 0), WAIT_TIMEOUT);
        EXPECT_TRUE(readablePath(target, observation.facts->gateParameter, observation.facts->gatePath));
        EXPECT_TRUE(readablePath(target, observation.facts->hookParameter, observation.facts->hookPath));
    }
    wchar_t name[64]{};
    wa::hook_ipc::mapName(observation.facts->pid, name, 64);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    EXPECT_NE(mapping, nullptr);
    if (mapping) {
        const auto* ring = static_cast<const wa::hook_ipc::Ring*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        EXPECT_NE(ring, nullptr);
        if (ring) { EXPECT_EQ(ring->installed, wa::hook_ipc::kInstallPending); UnmapViewOfFile(ring); }
        CloseHandle(mapping);
    }
    EXPECT_TRUE(TerminateProcess(controller.hProcess, 81));
    EXPECT_EQ(WaitForSingleObject(controller.hProcess, 5000), WAIT_OBJECT_0);
    DWORD code = STILL_ACTIVE;
    EXPECT_TRUE(GetExitCodeProcess(controller.hProcess, &code));
    EXPECT_EQ(code, 81u);
    if (target) {
        EXPECT_EQ(WaitForSingleObject(target, 5000), WAIT_OBJECT_0);
        EXPECT_TRUE(GetExitCodeProcess(target, &code));
        EXPECT_NE(code, STILL_ACTIVE);
        CloseHandle(target);
    }
    mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    const DWORD mappingError = mapping ? ERROR_SUCCESS : GetLastError();
    EXPECT_EQ(mapping, nullptr);
    EXPECT_EQ(mappingError, static_cast<DWORD>(ERROR_FILE_NOT_FOUND));
    if (mapping) CloseHandle(mapping);
    std::printf("pending_install mode=controller-kill pid=%u observed=%u target_exit=%lu mapping_gone=%d\n",
                observation.facts->pid, observation.facts->observed, code, !mapping);
    CloseHandle(controller.hThread);
    CloseHandle(controller.hProcess);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--pending-controller") return pendingController(argv[2]);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
