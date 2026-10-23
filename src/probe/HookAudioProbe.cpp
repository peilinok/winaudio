// Controlled, same-bitness audio targets for the hook acceptance check.
// This is a manual probe, not a device-dependent CI test.
#include "OnDemandAttach.h"
#include "HookedCallPod.h"
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
struct AudioStats {
    uint64_t callbacks = 0, failures = 0, discontinuities = 0, capturedFrames = 0;
    uint64_t nonzeroBytes = 0;
    uint32_t count = 0, sampleOverflow = 0;
    int64_t ticks[kSamples]{};
};
struct Shared {
    LONG ready = 0, stop = 0, phase = 0;
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
            if (process) WaitForSingleObject(process, 10000);
            UnmapViewOfFile(shared);
        }
        if (process) CloseHandle(process);
        if (map) CloseHandle(map);
        if (ring) UnmapViewOfFile(ring);
        if (ringMap) CloseHandle(ringMap);
    }
};

int victim(const wchar_t* name) {
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) return 2;
    auto* state = static_cast<Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!state) { CloseHandle(map); return 3; }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = com;
    {
        ComPtr<IMMDeviceEnumerator> enumer;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> output, input;
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* format = nullptr;
        UINT32 capacity = 0;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumer));
        if (SUCCEEDED(hr)) hr = enumer->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(output.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(input.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = output->GetMixFormat(&format);
        if (format) {
            state->sampleRate = format->nSamplesPerSec;
            state->channels = format->nChannels;
            state->bits = format->wBitsPerSample;
            state->formatTag = format->wFormatTag;
        }
        if (device) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id) {
                wcsncpy_s(state->endpoint, id, _TRUNCATE);
                CoTaskMemFree(id);
            }
        }
        const WORD alignment = format ? format->nBlockAlign : 0;
        if (SUCCEEDED(hr)) hr = output->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr);
        if (SUCCEEDED(hr)) hr = input->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, format, nullptr);
        if (format) CoTaskMemFree(format);
        if (SUCCEEDED(hr)) hr = output->GetService(IID_PPV_ARGS(&render));
        if (SUCCEEDED(hr)) hr = input->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(hr)) hr = output->GetBufferSize(&capacity);
        if (SUCCEEDED(hr)) hr = output->Start();
        if (SUCCEEDED(hr)) hr = input->Start();
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
            if (stats.callbacks % 20 == 0) {
                output->GetBufferSize(&capacity);
                ComPtr<IAudioRenderClient> currentRender;
                ComPtr<IAudioCaptureClient> currentCapture;
                if (FAILED(output->GetService(IID_PPV_ARGS(&currentRender)))) ++stats.failures;
                if (FAILED(input->GetService(IID_PPV_ARGS(&currentCapture)))) ++stats.failures;
            }
            Sleep(5);
        }
        if (input) input->Stop();
        if (output) output->Stop();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    UnmapViewOfFile(state);
    CloseHandle(map);
    return FAILED(hr) ? 4 : 0;
}

bool launch(Target& target, size_t index) {
    const std::wstring name = L"Local\\WinAudioHookAudioProbe-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(index);
    target.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   static_cast<DWORD>(sizeof(Shared)), name.c_str());
    if (!target.map) return false;
    target.shared = static_cast<Shared*>(MapViewOfFile(target.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!target.shared) return false;
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --victim \"" + name + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &info)) return false;
    CloseHandle(info.hThread);
    target.process = info.hProcess;
    target.pid = info.dwProcessId;
    for (int i = 0; i < 200 && !ReadAcquire(&target.shared->ready); ++i) {
        if (WaitForSingleObject(target.process, 25) == WAIT_OBJECT_0) return false;
    }
    return ReadAcquire(&target.shared->ready) && SUCCEEDED(target.shared->error);
}

bool attach(Target& target) {
    const auto result = target.attach.start(target.pid, {}, wa::PipelineFlow::Render);
    if (!result) { std::printf("attach failed: %s\n", result.message.c_str()); return false; }
    if (!target.ring) {
        wchar_t name[64]{};
        wa::hook_ipc::mapName(target.pid, name, 64);
        target.ringMap = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!target.ringMap) return false;
        target.ring = static_cast<wa::hook_ipc::Ring*>(
            MapViewOfFile(target.ringMap, FILE_MAP_READ, 0, 0, sizeof(wa::hook_ipc::Ring)));
        if (!target.ring) return false;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, target.pid);
    bool found = false;
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(snapshot, &module)) do {
            if (_wcsicmp(module.szModule, L"WinAudioHook-5.dll") == 0) {
                std::wprintf(L"loaded pid=%lu path=%ls\n", target.pid, module.szExePath);
                found = true;
            }
        } while (Module32NextW(snapshot, &module));
        CloseHandle(snapshot);
    }
    return found;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && !std::wcscmp(argv[1], L"--victim")) return victim(argv[2]);
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
