#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "StartupGateHarness.h"
#include "HookedCallPod.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>

namespace {
IAudioClient* retainedClient = nullptr;
IAudioClient* retainedMainClient = nullptr;
IAudioClient* retainedWorkerClient = nullptr;
bool retainedCom = false;
HANDLE workerStop = nullptr;
DWORD lifetime() { return wcsstr(GetCommandLineW(), L"--long-lived") ? 600000u : 20000u; }
std::wstring token() {
    const wchar_t* p = wcsstr(GetCommandLineW(), L"--token=");
    if (!p) return {};
    p += 8;
    const wchar_t* end = wcschr(p, L' ');
    return end ? std::wstring(p, end) : std::wstring(p);
}
uint32_t emittedRows() {
    wchar_t name[64]{};
    wa::hook_ipc::mapName(GetCurrentProcessId(), name, 64);
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!map) return 0;
    auto* ring = static_cast<const wa::hook_ipc::Ring*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(wa::hook_ipc::Ring)));
    const uint32_t rows = ring ? ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&ring->writeIndex)) : 0;
    if (ring) UnmapViewOfFile(ring);
    CloseHandle(map);
    return rows;
}

DWORD WINAPI exitWorker(void*) {
    // This worker can request a real target-owned exit while EXE entry is held.
    // The TLS callback only creates it; no COM or wait occurs under loader lock.
    const wchar_t* p = wcsstr(GetCommandLineW(), L"--token=");
    if (!p) return 1;
    p += 8;
    wchar_t name[256]{};
    unsigned n = 0;
    while (p[n] && p[n] != L' ' && n < 230) { name[n] = p[n]; ++n; }
    lstrcatW(name, L"-exit");
    HANDLE request = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!request) return 2;
    const DWORD waited = WaitForSingleObject(request, 15000);
    CloseHandle(request);
    if (waited == WAIT_OBJECT_0) ExitProcess(75);
    return 3;
}
void NTAPI tlsCallback(void*, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        const wchar_t* p = wcsstr(GetCommandLineW(), L"--token=");
        wchar_t name[256]{};
        if (p) {
            p += 8;
            unsigned n = 0;
            while (p[n] && p[n] != L' ' && n < 230) { name[n] = p[n]; ++n; }
            lstrcatW(name, L"-map");
            HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            auto* shared = map ? static_cast<wa::startup_probe::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
            if (shared) {
                shared->pid = GetCurrentProcessId();
                LARGE_INTEGER counter{};
                QueryPerformanceCounter(&counter);
                shared->tlsCounter = counter.QuadPart;
                InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->tlsEntered), 1);
                UnmapViewOfFile(shared);
            }
            if (map) CloseHandle(map);
        }
    }
    if (reason == DLL_PROCESS_ATTACH && wcsstr(GetCommandLineW(), L"--remote-gate")) {
        // Test fixture only. The gate DLL reads this when loaded remotely
        // while EXE entry is held; production Hook code has no test gate.
        const wchar_t* p = wcsstr(GetCommandLineW(), L"--token=");
        wchar_t base[160]{};
        if (p) {
            p += 8;
            unsigned n = 0;
            while (p[n] && p[n] != L' ' && n < 150) { base[n] = p[n]; ++n; }
            SetEnvironmentVariableW(L"WA_REMOTE_GATE", base);
        }
    }
    if (reason == DLL_PROCESS_ATTACH && wcsstr(GetCommandLineW(), L"--debugbreak-before-entry")) {
        DebugBreak();
        ExitProcess(76);  // Reaching this proves that a target exception was swallowed.
    }
    if (reason == DLL_PROCESS_ATTACH && wcsstr(GetCommandLineW(), L"--hold-before-entry"))
        LoadLibraryW(L"RemoteInjectionGate.dll");
    if (reason == DLL_PROCESS_ATTACH && wcsstr(GetCommandLineW(), L"--exit-worker")) {
        HANDLE worker = CreateThread(nullptr, 0, exitWorker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    }
}
#ifdef _WIN64
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:startupTls")
#else
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_startupTls")
#endif
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK startupTls = tlsCallback;
#pragma const_seg()

HRESULT audioCalls(wa::startup_probe::Shared& shared, unsigned stage = 0) {
    const auto previousCalls = shared.calls;
    using Microsoft::WRL::ComPtr;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::printf("target CoInitializeEx hr=0x%08lX\n", initialized);
    if (FAILED(initialized)) return initialized;
    HRESULT result = E_FAIL;
    shared.sourceThread = GetCurrentThreadId();
    auto& marker = shared.audioStages[stage];
    marker.thread = GetCurrentThreadId();
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        std::printf("target CoCreateInstance/MMDeviceEnumerator hr=0x%08lX\n", result);
        if (SUCCEEDED(result)) result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        std::printf("target GetDefaultAudioEndpoint eRender/eConsole hr=0x%08lX\n", result);
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        shared.firstCallCounter = counter.QuadPart;
        marker.counter = counter.QuadPart;
        if (SUCCEEDED(result)) {
            result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf()));
            shared.sourceResults[0] = result;
            shared.clientAddress = reinterpret_cast<uintptr_t>(client.Get());
            marker.client = shared.clientAddress;
            ++shared.calls;
            std::printf("target Activate/IAudioClient hr=0x%08lX\n", result);
        }
        WAVEFORMATEX* format = nullptr;
        if (SUCCEEDED(result)) result = client->GetMixFormat(&format);
        if (SUCCEEDED(result)) {
            shared.sampleRate = format->nSamplesPerSec;
            shared.bits = format->wBitsPerSample;
            shared.channels = format->nChannels;
            result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr);
            shared.sourceResults[1] = result;
            ++shared.calls;
            std::printf("target Initialize shared flags=0 duration=1000000 hr=0x%08lX\n", result);
        }
        CoTaskMemFree(format);
        if (SUCCEEDED(result)) {
            result = client->Start();
            shared.sourceResults[2] = result;
            ++shared.calls;
            std::printf("target Start hr=0x%08lX\n", result);
        }
        if (SUCCEEDED(result)) {
            result = client->Stop();
            shared.sourceResults[3] = result;
            ++shared.calls;
            std::printf("target Stop hr=0x%08lX\n", result);
        }
        if (!wcsstr(GetCommandLineW(), L"--release-before-main")) {
            if (stage == 0) retainedClient = client.Detach();
            else if (stage == 1) retainedMainClient = client.Detach();
            else retainedWorkerClient = client.Detach();
        }
        else {
            shared.releaseRowsBefore = emittedRows();
            client.Reset();
            shared.releaseRowsAfter = emittedRows();
        }
    }
    if (wcsstr(GetCommandLineW(), L"--release-before-main")) {
        CoUninitialize();
        shared.uninitRowsAfter = emittedRows();
    }
    else retainedCom = true;
    marker.calls = shared.calls - previousCalls;
    for (unsigned i = 0; i < 4; ++i) marker.results[i] = shared.sourceResults[i];
    marker.sampleRate = shared.sampleRate;
    marker.bits = shared.bits;
    marker.channels = shared.channels;
    return result;
}
DWORD WINAPI audioWorker(void* context) {
    auto& shared = *static_cast<wa::startup_probe::Shared*>(context);
    const auto result = audioCalls(shared, 2);
    SetEvent(workerStop);
    const auto base = token();
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (base + L"-stop").c_str());
    if (stop) { WaitForSingleObject(stop, lifetime()); CloseHandle(stop); }
    if (retainedWorkerClient) { retainedWorkerClient->Release(); retainedWorkerClient = nullptr; }
    CoUninitialize();
    return FAILED(result) ? 8u : 0u;
}
struct Initializer {
    Initializer() {
        const auto base = token();
        HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (base + L"-map").c_str());
        if (!map) return;
        auto* shared = static_cast<wa::startup_probe::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (shared) {
            shared->pid = GetCurrentProcessId();
            auto* module = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
            shared->imageBase = reinterpret_cast<uintptr_t>(module);
            shared->entryRva = nt->OptionalHeader.AddressOfEntryPoint;
            shared->entryByte = module[shared->entryRva];
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            shared->crtCounter = counter.QuadPart;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->crtEntered), 1);
            if (wcsstr(GetCommandLineW(), L"--audio")) shared->audioResult = audioCalls(*shared);
            UnmapViewOfFile(shared);
        }
        CloseHandle(map);
    }
} initialize;
}

int wmain() {
    const auto base = token();
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (base + L"-map").c_str());
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (base + L"-ready").c_str());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (base + L"-stop").c_str());
    if (!map || !ready || !stop) return 2;
    auto* shared = static_cast<wa::startup_probe::Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!shared) return 3;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    shared->mainCounter = counter.QuadPart;
    wcsncpy_s(shared->commandLine, GetCommandLineW(), _TRUNCATE);
    GetCurrentDirectoryW(static_cast<DWORD>(std::size(shared->directory)), shared->directory);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->mainEntered), 1);
    HMODULE lateModule = nullptr;
    if (wcsstr(GetCommandLineW(), L"--late-system-dll")) {
        shared->lateDllWasAbsent = GetModuleHandleW(L"winhttp.dll") == nullptr;
        wchar_t library[MAX_PATH]{};
        const auto length = GetSystemDirectoryW(library, MAX_PATH);
        if (!length || length + 13 >= MAX_PATH) return 11;
        wcscat_s(library, L"\\winhttp.dll");
        lateModule = LoadLibraryW(library);
        shared->lateDllLoaded = lateModule != nullptr;
        QueryPerformanceCounter(&counter);
        shared->lateDllCounter = counter.QuadPart;
        std::printf("target late_dll=winhttp.dll absent_before=%u loaded=%u qpc=%lld\n",
            shared->lateDllWasAbsent, shared->lateDllLoaded, shared->lateDllCounter);
    }
    HANDLE worker = nullptr;
    if (wcsstr(GetCommandLineW(), L"--audio-all-stages")) {
        shared->audioResult = audioCalls(*shared, 1);
        workerStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker = CreateThread(nullptr, 0, audioWorker, shared, 0, nullptr);
        if (!worker || WaitForSingleObject(workerStop, 10000) != WAIT_OBJECT_0) return 9;
    }
    std::printf("target main pid=%lu crt=%u calls=%u hr=0x%08lX\n", GetCurrentProcessId(), shared->crtEntered,
                shared->calls, shared->audioResult);
    SetEvent(ready);
    const DWORD waited = WaitForSingleObject(stop, lifetime());
    if (worker) {
        if (WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0) return 10;
        DWORD code = STILL_ACTIVE;
        GetExitCodeThread(worker, &code);
        CloseHandle(worker);
        CloseHandle(workerStop);
        if (code) return static_cast<int>(code);
    }
    if (retainedClient) { retainedClient->Release(); retainedClient = nullptr; }
    if (retainedMainClient) { retainedMainClient->Release(); retainedMainClient = nullptr; CoUninitialize(); }
    if (retainedCom) { CoUninitialize(); retainedCom = false; }
    if (lateModule) FreeLibrary(lateModule);
    UnmapViewOfFile(shared);
    CloseHandle(map);
    CloseHandle(ready);
    CloseHandle(stop);
    return waited == WAIT_OBJECT_0 ? 0 : 4;
}
