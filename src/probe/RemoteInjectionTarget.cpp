#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include "RemoteInjectionHarness.h"
#include "OnDemandAttach.h"
#include <string>
#include <future>

namespace {
HANDLE gate = nullptr;
HANDLE entered = nullptr;
wa::injection_probe::Shared* shared = nullptr;

DWORD WINAPI delayedRead(void* parameter) {
    SetEvent(entered);
    if (WaitForSingleObject(gate, 10000) != WAIT_OBJECT_0) return 3;
    uint32_t value = 0;
    SIZE_T read = 0;
    const BOOL ok = ReadProcessMemory(GetCurrentProcess(), parameter, &value, sizeof(value), &read);
    shared->value = value;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->readOk),
                        ok && read == sizeof(value) ? 1 : 2);
    return 0x7351;
}

uint32_t countThreads(uint32_t pid) {
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
}

int wmain(int argc, wchar_t** argv) {
    if ((argc == 3 || argc == 4) && std::wstring(argv[1]) == L"--missing-hook") {
        const auto pid = static_cast<uint32_t>(wcstoul(argv[2], nullptr, 10));
        wa::OnDemandAttach first, second;
        wa::Result a;
        if (argc == 4) {
            std::atomic_bool cancelled{false};
            const uint32_t initial = countThreads(pid);
            auto started = std::async(std::launch::async, [&] {
                return first.start(pid, {}, wa::PipelineFlow::Render, &cancelled);
            });
            const ULONGLONG threadDeadline = GetTickCount64() + 5000;
            while (countThreads(pid) <= initial && GetTickCount64() < threadDeadline) Sleep(1);
            cancelled.store(true);
            a = started.get();
            if (a.code != static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED))) return 7;
            const std::wstring mapName = std::wstring(argv[3]) + L"-map";
            HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapName.c_str());
            if (!map) return 8;
            auto* view = static_cast<wa::injection_probe::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
            if (!view) { CloseHandle(map); return 9; }
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&view->readOk), 1);
            const ULONGLONG cleanupDeadline = GetTickCount64() + 5000;
            while (wa::OnDemandAttach::reapPending() != 0 && GetTickCount64() < cleanupDeadline) Sleep(1);
            const bool reclaimed = wa::OnDemandAttach::reapPending() == 0;
            UnmapViewOfFile(view);
            CloseHandle(map);
            if (!reclaimed) return 10;
        } else a = first.start(pid);
        const auto b = second.start(pid);
        const long missing = static_cast<long>(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
        const long firstExpected = argc == 4 ? static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)) : missing;
        return !a && !b && a.code == firstExpected && b.code == missing ? 0 : 5;
    }
    if (argc != 2 && argc != 3) return 1;
    const std::wstring base = argv[1];
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (base + L"-map").c_str());
    gate = OpenEventW(SYNCHRONIZE, FALSE, (base + L"-gate").c_str());
    entered = OpenEventW(EVENT_MODIFY_STATE, FALSE, (base + L"-entered").c_str());
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (base + L"-ready").c_str());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (base + L"-stop").c_str());
    if (!mapping || !gate || !entered || !ready || !stop) return 2;
    shared = static_cast<wa::injection_probe::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!shared) return 3;
    shared->entry = reinterpret_cast<uintptr_t>(&delayedRead);
    SetEvent(ready);
    if (argc == 3 && std::wstring(argv[2]) == L"--exit-on-go") {
        HANDLE exitGo = OpenEventW(SYNCHRONIZE, FALSE, (base + L"-exit").c_str());
        if (!exitGo) return 11;
        const DWORD waited = WaitForSingleObject(exitGo, 30000);
        CloseHandle(exitGo);
        return waited == WAIT_OBJECT_0 ? 0 : 12;
    }
    if (argc == 3 && std::wstring(argv[2]) == L"--hold-loader") {
        SetEnvironmentVariableW(L"WA_REMOTE_GATE", base.c_str());
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dll = path;
        dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"RemoteInjectionGate.dll";
        if (!LoadLibraryW(dll.c_str())) return 6;
    }
    const DWORD waited = WaitForSingleObject(stop, 180000);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    CloseHandle(gate);
    CloseHandle(entered);
    CloseHandle(ready);
    CloseHandle(stop);
    return waited == WAIT_OBJECT_0 ? 0 : 4;
}
