// Controlled, same-bitness audio targets for the hook acceptance check.
// This is a manual probe, not a device-dependent CI test.
#include "OnDemandAttach.h"
#include "HookedCallPod.h"
#include "Log.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t kPhases = 7, kSamples = 4096;
HRESULT observe(const char* operation, HRESULT hr) {
    WA_LOG(FAILED(hr) ? wa::log::Level::Warn : wa::log::Level::Debug,
           "HookAudioProbe", operation, "control", wa::log::hrName(hr));
    return hr;
}
bool observeWin32(const char* operation, bool ok) {
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    observe(operation, ok ? S_OK : HRESULT_FROM_WIN32(error));
    return ok;
}
struct ProbeLog {
    ProbeLog() {
        wa::log::init();
        wa::log::setThreadName("probe");
        wa::log::addStderrSink();
    }
    ~ProbeLog() { wa::log::shutdown(); }
};
struct AudioStats {
    uint64_t callbacks = 0, failures = 0, discontinuities = 0, capturedFrames = 0;
    uint64_t nonzeroBytes = 0;
    uint32_t count = 0, sampleOverflow = 0;
    int64_t ticks[kSamples]{};
};
struct Shared {
    LONG ready = 0, stop = 0, phase = 0;
    uint32_t controlEvery = 20;
    HRESULT error = S_OK;
    DWORD sampleRate = 0;
    WORD channels = 0, bits = 0, formatTag = 0;
    wchar_t endpoint[256]{};
    AudioStats stats[kPhases]{};
};
struct Target {
    HANDLE map = nullptr, process = nullptr, ringMap = nullptr;
    DWORD pid = 0;
    Shared* shared = nullptr;
    wa::hook_ipc::Ring* ring = nullptr;
    wa::OnDemandAttach attach;
    ~Target() {
        attach.stop();
        if (shared) {
            InterlockedExchange(&shared->stop, 1);
            if (process) observe("WaitForSingleObject/cleanup",
                WaitForSingleObject(process, 10000) == WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            observeWin32("UnmapViewOfFile/shared", UnmapViewOfFile(shared) != FALSE);
        }
        if (process) observeWin32("CloseHandle/process", CloseHandle(process) != FALSE);
        if (map) observeWin32("CloseHandle/map", CloseHandle(map) != FALSE);
        if (ring) observeWin32("UnmapViewOfFile/ring", UnmapViewOfFile(ring) != FALSE);
        if (ringMap) observeWin32("CloseHandle/ringMap", CloseHandle(ringMap) != FALSE);
    }
};

int victim(const wchar_t* name) {
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!observeWin32("OpenFileMappingW/victim", map != nullptr)) return 2;
    auto* state = static_cast<Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!observeWin32("MapViewOfFile/victim", state != nullptr)) {
        observeWin32("CloseHandle/victim", CloseHandle(map) != FALSE); return 3;
    }
    const HRESULT com = observe("CoInitializeEx", CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HRESULT hr = com;
    {
        ComPtr<IMMDeviceEnumerator> enumer;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> output, input;
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* format = nullptr;
        UINT32 capacity = 0;
        if (SUCCEEDED(hr)) hr = observe("CoCreateInstance", CoCreateInstance(__uuidof(MMDeviceEnumerator),
            nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumer)));
        if (SUCCEEDED(hr)) hr = observe("GetDefaultAudioEndpoint", enumer->GetDefaultAudioEndpoint(eRender, eConsole, &device));
        if (SUCCEEDED(hr)) hr = observe("Activate/render", device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(output.GetAddressOf())));
        if (SUCCEEDED(hr)) hr = observe("Activate/capture", device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(input.GetAddressOf())));
        if (SUCCEEDED(hr)) hr = observe("GetMixFormat", output->GetMixFormat(&format));
        if (format) {
            state->sampleRate = format->nSamplesPerSec;
            state->channels = format->nChannels;
            state->bits = format->wBitsPerSample;
            state->formatTag = format->wFormatTag;
        }
        if (device) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(observe("GetId", device->GetId(&id))) && id) {
                wcsncpy_s(state->endpoint, id, _TRUNCATE);
                CoTaskMemFree(id);
            }
        }
        const WORD alignment = format ? format->nBlockAlign : 0;
        if (SUCCEEDED(hr)) hr = observe("Initialize/render", output->Initialize(
            AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr));
        if (SUCCEEDED(hr)) hr = observe("Initialize/capture", input->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, format, nullptr));
        if (format) CoTaskMemFree(format);
        if (SUCCEEDED(hr)) hr = observe("GetService/render", output->GetService(IID_PPV_ARGS(&render)));
        if (SUCCEEDED(hr)) hr = observe("GetService/capture", input->GetService(IID_PPV_ARGS(&capture)));
        if (SUCCEEDED(hr)) hr = observe("GetBufferSize", output->GetBufferSize(&capacity));
        if (SUCCEEDED(hr)) hr = observe("Start/render", output->Start());
        if (SUCCEEDED(hr)) hr = observe("Start/capture", input->Start());
        WA_LOG(wa::log::Level::Info, "HookAudioProbe", "ready", std::to_string(GetCurrentProcessId()), wa::log::hrName(hr));
        state->error = hr;
        InterlockedExchange(&state->ready, 1);
        while (SUCCEEDED(hr) && !ReadAcquire(&state->stop)) {
            const auto phase = static_cast<size_t>(ReadAcquire(&state->phase));
            auto& stats = state->stats[phase];
            LARGE_INTEGER start{}, end{};
            QueryPerformanceCounter(&start);
            UINT32 padding = 0;
            if (FAILED(output->GetCurrentPadding(&padding))) ++stats.failures;
            const UINT32 available = capacity > padding ? capacity - padding : 0;
            if (available) {
                BYTE* data = nullptr;
                if (FAILED(render->GetBuffer(available, &data))) ++stats.failures;
                else if (FAILED(render->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT))) ++stats.failures;
            }
            for (;;) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                const HRESULT got = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (got == AUDCLNT_S_BUFFER_EMPTY) break;
                if (FAILED(got)) { ++stats.failures; break; }
                stats.capturedFrames += frames;
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++stats.discontinuities;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    for (size_t i = 0; i < static_cast<size_t>(frames) * alignment; ++i)
                        if (data[i]) ++stats.nonzeroBytes;
                }
                if (FAILED(capture->ReleaseBuffer(frames))) ++stats.failures;
            }
            QueryPerformanceCounter(&end);
            ++stats.callbacks;
            if (stats.count < kSamples) stats.ticks[stats.count++] = end.QuadPart - start.QuadPart;
            else ++stats.sampleOverflow;
            // A low-frequency control call proves pump-off does not gate control.
            if (stats.callbacks % state->controlEvery == 0) {
                observe("GetBufferSize/periodic", output->GetBufferSize(&capacity));
                ComPtr<IAudioRenderClient> currentRender;
                ComPtr<IAudioCaptureClient> currentCapture;
                if (FAILED(observe("GetService/render/periodic", output->GetService(IID_PPV_ARGS(&currentRender))))) ++stats.failures;
                if (FAILED(observe("GetService/capture/periodic", input->GetService(IID_PPV_ARGS(&currentCapture))))) ++stats.failures;
            }
            Sleep(5);
        }
        if (input) observe("Stop/capture", input->Stop());
        if (output) observe("Stop/render", output->Stop());
    }
    if (SUCCEEDED(com)) { CoUninitialize(); observe("CoUninitialize", S_OK); }
    observeWin32("UnmapViewOfFile/victim", UnmapViewOfFile(state) != FALSE);
    observeWin32("CloseHandle/victim", CloseHandle(map) != FALSE);
    return FAILED(hr) ? 4 : 0;
}

bool launch(Target& target, size_t index, uint32_t controlEvery = 20) {
    const std::wstring name = L"Local\\WinAudioHookAudioProbe-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(index);
    target.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   static_cast<DWORD>(sizeof(Shared)), name.c_str());
    if (!observeWin32("CreateFileMappingW", target.map != nullptr)) return false;
    target.shared = static_cast<Shared*>(MapViewOfFile(target.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!observeWin32("MapViewOfFile/controller", target.shared != nullptr)) return false;
    target.shared->controlEvery = controlEvery;
    wchar_t self[MAX_PATH]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, self, MAX_PATH);
    observeWin32("GetModuleFileNameW", pathLength != 0);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --victim \"" + name + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    if (!observeWin32("CreateProcessW", CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE)) return false;
    observeWin32("CloseHandle/thread", CloseHandle(info.hThread) != FALSE);
    target.process = info.hProcess;
    target.pid = info.dwProcessId;
    for (int i = 0; i < 200 && !ReadAcquire(&target.shared->ready); ++i) {
        if (WaitForSingleObject(target.process, 25) == WAIT_OBJECT_0) return false;
    }
    const bool ready = ReadAcquire(&target.shared->ready) && SUCCEEDED(target.shared->error);
    observe("target-ready", ready ? S_OK : target.shared->error == S_OK ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : target.shared->error);
    return ready;
}

bool attach(Target& target) {
    const auto result = target.attach.start(target.pid, {}, wa::PipelineFlow::Render);
    if (!result) { std::printf("attach failed: %s\n", result.message.c_str()); return false; }
    if (!target.ring) {
        wchar_t name[64]{};
        wa::hook_ipc::mapName(target.pid, name, 64);
        target.ringMap = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!observeWin32("OpenFileMappingW/ring", target.ringMap != nullptr)) return false;
        target.ring = static_cast<wa::hook_ipc::Ring*>(
            MapViewOfFile(target.ringMap, FILE_MAP_READ, 0, 0, sizeof(wa::hook_ipc::Ring)));
        if (!observeWin32("MapViewOfFile/ring", target.ring != nullptr)) return false;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, target.pid);
    observeWin32("CreateToolhelp32Snapshot", snapshot != INVALID_HANDLE_VALUE);
    bool found = false;
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (observeWin32("Module32FirstW", Module32FirstW(snapshot, &module) != FALSE)) do {
            if (_wcsicmp(module.szModule, L"WinAudioHook-5.dll") == 0) {
                std::wprintf(L"loaded pid=%lu path=%ls\n", target.pid, module.szExePath);
                found = true;
            }
        } while (Module32NextW(snapshot, &module));
        const DWORD moduleError = GetLastError();
        observe("Module32NextW", moduleError == ERROR_NO_MORE_FILES ? S_OK : HRESULT_FROM_WIN32(moduleError));
        observeWin32("CloseHandle/snapshot", CloseHandle(snapshot) != FALSE);
    }
    return found;
}
}

int wmain(int argc, wchar_t** argv) {
    ProbeLog logging;
    if (argc == 3 && !std::wcscmp(argv[1], L"--victim")) return victim(argv[2]);
    if (argc == 3 && !std::wcscmp(argv[1], L"--continuous")) {
        const unsigned seconds = static_cast<unsigned>(std::wcstoul(argv[2], nullptr, 10));
        if (!seconds) return 20;
        std::array<Target, 2> targets;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (!launch(targets[i], i, 400)) return 21;
            const auto& s = *targets[i].shared;
            std::printf("ready target=%zu pid=%lu sample_rate=%lu channels=%u bits=%u endpoint=%ls\n",
                        i, targets[i].pid, s.sampleRate, s.channels, s.bits, s.endpoint);
        }
        std::fflush(stdout);
        // This mode owns the targets but does not attach; the production GUI does.
        for (unsigned i = 0; i < seconds; ++i) {
            Sleep(1000);
            for (auto& t : targets)
                if (WaitForSingleObject(t.process, 0) == WAIT_OBJECT_0) return 22;
        }
        for (auto& t : targets) InterlockedExchange(&t.shared->stop, 1);
        bool ok = true;
        for (auto& t : targets) {
            if (WaitForSingleObject(t.process, 10000) != WAIT_OBJECT_0) return 23;
            const auto& s = t.shared->stats[0];
            std::printf("done pid=%lu callbacks=%llu failures=%llu discontinuities=%llu captured_frames=%llu "
                        "timing_samples=%u timing_overflow=%u\n", t.pid, s.callbacks, s.failures,
                        s.discontinuities, s.capturedFrames, s.count, s.sampleOverflow);
            // The bounded timing reservoir is not an IPC or audio overflow counter.
            if (!s.callbacks || !s.capturedFrames || s.failures) ok = false;
        }
        return ok ? 0 : 24;
    }
    constexpr const char* names[kPhases] = {"no-attach", "capture-off", "pump-off", "pump-on",
                                          "detached", "reattached", "other-pid-detached"};
    std::array<Target, 2> targets;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!launch(targets[i], i)) { std::printf("target setup failed index=%zu\n", i); return 10; }
        const auto& s = *targets[i].shared;
        std::printf("target=%zu endpoint=%ls sample_rate=%lu channels=%u bits=%u format_tag=0x%X\n",
                    i, s.endpoint, s.sampleRate, s.channels, s.bits, s.formatTag);
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    std::printf("qpc_frequency=%lld; two fresh same-bitness targets; silent render + actual endpoint loopback\n",
                frequency.QuadPart);
    bool ok = true;
    for (size_t phase = 0; phase < kPhases; ++phase) {
        if (phase == 1 || phase == 5) for (auto& t : targets) if (!attach(t)) return 11;
        if (phase == 4) for (auto& t : targets) t.attach.stop();
        if (phase == 6) targets[0].attach.stop();
        for (auto& t : targets) {
            if (t.attach.attached()) {
                t.attach.setCaptureEnabled(phase != 1);
                t.attach.setPumpEnabled(phase != 2);
            }
            InterlockedExchange(&t.shared->phase, static_cast<LONG>(phase));
        }
        // Allow the explicitly permitted boundary calls to finish before
        // checking stable-disabled index deltas. No strict drain is implied.
        Sleep(100);
        std::array<ULONG, 2> controlStart{}, pumpStart{};
        for (size_t i = 0; i < targets.size(); ++i) if (targets[i].ring) {
            controlStart[i] = ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&targets[i].ring->writeIndex));
            pumpStart[i] = ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&targets[i].ring->pumpWriteIndex));
        }
        for (int i = 0; i < 32; ++i) {
            Sleep(250);
            for (auto& t : targets) {
                if (WaitForSingleObject(t.process, 0) == WAIT_OBJECT_0) return 12;
            }
        }
        for (size_t i = 0; i < targets.size(); ++i) {
            auto& t = targets[i];
            const auto control = t.attach.drain();
            const auto pump = t.attach.pumpRing();
            const bool recording = t.attach.attached() && phase != 1;
            const bool pumping = recording && phase != 2;
            size_t captureCalls = 0, renderCalls = 0;
            for (const auto& call : pump) {
                if (call.iface == "IAudioCaptureClient") ++captureCalls;
                if (call.iface == "IAudioRenderClient") ++renderCalls;
            }
            const ULONG controlDelta = t.ring ? ReadULongAcquire(
                reinterpret_cast<const volatile ULONG*>(&t.ring->writeIndex)) - controlStart[i] : 0;
            const ULONG pumpDelta = t.ring ? ReadULongAcquire(
                reinterpret_cast<const volatile ULONG*>(&t.ring->pumpWriteIndex)) - pumpStart[i] : 0;
            std::printf("phase=%s target=%zu pid=%lu attached=%d control=%zu pump=%zu "
                        "control_delta=%lu pump_delta=%lu capture_calls=%zu render_calls=%zu\n",
                        names[phase], i, t.pid, t.attach.attached(), control.size(), pump.size(),
                        controlDelta, pumpDelta, captureCalls, renderCalls);
            if ((recording && control.empty()) || (pumping && pump.empty()) || (!pumping && !pump.empty())) ok = false;
            if ((!recording && controlDelta) || (!pumping && pumpDelta) ||
                (pumping && (!captureCalls || !renderCalls))) ok = false;
        }
    }
    for (auto& t : targets) InterlockedExchange(&t.shared->stop, 1);
    for (auto& t : targets) {
        if (WaitForSingleObject(t.process, 10000) != WAIT_OBJECT_0) return 13;
    }
    std::printf("phase,target,callbacks,failures,discontinuities,captured_frames,nonzero_bytes,samples,overflow,p50_us,p95_us,p99_us\n");
    for (size_t phase = 0; phase < kPhases; ++phase) for (size_t i = 0; i < targets.size(); ++i) {
        const auto& s = targets[i].shared->stats[phase];
        std::vector<int64_t> ticks(s.ticks, s.ticks + s.count);
        std::sort(ticks.begin(), ticks.end());
        const auto rank = [&](size_t p) { return ticks.empty() ? 0.0 :
            ticks[(ticks.size() * p + 99) / 100 - 1] * 1e6 / frequency.QuadPart; };
        std::printf("%s,%zu,%llu,%llu,%llu,%llu,%llu,%u,%u,%.3f,%.3f,%.3f\n", names[phase], i,
            s.callbacks, s.failures, s.discontinuities, s.capturedFrames, s.nonzeroBytes,
            s.count, s.sampleOverflow, rank(50), rank(95), rank(99));
        if (!s.callbacks || !s.capturedFrames || s.failures || s.sampleOverflow) ok = false;
    }
    std::printf("result=%s; inspect per-phase audio tails and startup discontinuities separately\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 14;
}
