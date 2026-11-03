#include "HookModuleIdentity.h"
// Controlled, same-bitness audio targets for the hook acceptance check.
// This is a manual probe, not a device-dependent CI test.
#include "OnDemandAttach.h"
#include "HookedCallPod.h"
#include "Log.h"
#include "../gui/PipelineAttachUiModel.h"
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

int victim(const wchar_t* name, float controlVolume = -1.f) {
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
                if (controlVolume >= 0.f) {
                    ComPtr<ISimpleAudioVolume> volume;
                    const auto got = observe("GetService/volume/periodic", output->GetService(IID_PPV_ARGS(&volume)));
                    if (FAILED(got)) ++stats.failures;
                    else if (FAILED(observe("SetMasterVolume/periodic", volume->SetMasterVolume(controlVolume, nullptr))))
                        ++stats.failures;
                }
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

bool launch(Target& target, size_t index, uint32_t controlEvery = 20, float controlVolume = -1.f) {
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
    if (controlVolume >= 0.f) command += L" " + std::to_wstring(controlVolume);
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

bool loadedHook(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    observeWin32("CreateToolhelp32Snapshot", snapshot != INVALID_HANDLE_VALUE);
    bool found = false;
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (observeWin32("Module32FirstW", Module32FirstW(snapshot, &module) != FALSE)) do {
            if (wa::isHookModuleName(module.szModule)) {
                std::wprintf(L"loaded pid=%lu path=%ls layout=%u ring_bytes=%zu\n",
                    pid, module.szExePath, wa::hook_ipc::kRingLayout, sizeof(wa::hook_ipc::Ring));
                found = true;
            }
        } while (Module32NextW(snapshot, &module));
        const DWORD moduleError = GetLastError();
        observe("Module32NextW", moduleError == ERROR_NO_MORE_FILES ? S_OK : HRESULT_FROM_WIN32(moduleError));
        observeWin32("CloseHandle/snapshot", CloseHandle(snapshot) != FALSE);
    }
    return found;
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
    return loadedHook(target.pid);
}

int segments(bool methods = false) {
    std::array<Target, 2> targets;
    for (size_t i = 0; i < targets.size(); ++i)
        if (!launch(targets[i], i, 4, i ? 0.75f : 0.25f)) return 30;
    wa::PipelineAttachController controller;
    wa::CallLogStore store;
    wa::pipeline_attach_ui::SegmentSelection selection;
    bool ok = true;
    unsigned published = 0;
    const uint32_t initialMasks[] = {wa::methodBit(wa::MonitorMethod::SetMasterVolume),
        wa::methodBit(wa::MonitorMethod::GetService) | wa::methodBit(wa::MonitorMethod::CaptureGetBuffer)};
    const auto consume = [&] {
        for (auto& batch : controller.poll()) {
            for (const auto& event : batch.events) {
                std::printf("event pid=%u created=%llu token=%llu kind=%d exited=%d hr=%ld snapshot_ms=%lld\n",
                    event.pid, event.instance.created, event.token, static_cast<int>(event.kind), event.exited,
                    event.result.code, event.snapshotMs);
                if (!event.result) ok = false;
                if (event.kind == wa::PipelineAttachController::EventKind::Configured) ++published;
            }
            selection.apply(store, std::move(batch));
        }
    };
    const auto wait = [&](const auto& predicate) {
        const auto deadline = GetTickCount64() + 10000;
        do {
            consume();
            if (predicate()) { consume(); return true; }
            Sleep(10);
        } while (GetTickCount64() < deadline);
        return false;
    };
    std::array<uint64_t, 2> ids{};
    std::array<wa::ProcessInstance, 2> instances{};
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!wa::queryProcessInstance(targets[i].pid, instances[i])) return 31;
        if (!controller.start(targets[i].pid, {}, wa::PipelineFlow::Render, instances[i],
            methods ? initialMasks[i] : UINT32_MAX)) return 32;
        if (!wait([&] { return !controller.snapshot().pendingPid; })) return 33;
        ids[i] = store.latestAttach(instances[i]);
        if (!ids[i]) return 34;
        if (!loadedHook(targets[i].pid)) return 53;
    }
    if (methods) {
        store.setPumpVisible(true);
        if (!wait([&] { return store.segment(ids[0])->count > 5 && store.segment(ids[1])->count > 5 && !store.pumpEntries().empty(); })) return 44;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (store.segment(ids[i])->methods != initialMasks[i]) ok = false;
            wa::call_log_filter::FilterOpts query; query.attach = ids[i];
            const auto view = store.view(query);
            size_t controls = 0, buffers = 0;
            for (size_t row = 0; row < view.size(); ++row) {
                const auto& call = view.at(row)->call;
                if (i == 0 && call.method != "SetMasterVolume") ok = false;
                if (i == 1 && call.method != "GetService" && !(call.iface == "IAudioCaptureClient" && call.method == "GetBuffer")) ok = false;
                if (call.pump) ++buffers; else ++controls;
            }
            std::printf("initial_selection pid=%lu mask=%u controls=%zu buffers=%zu format=%lu/%u/%u/tag=%u endpoint=%ls\n",
                targets[i].pid, initialMasks[i], controls, buffers, targets[i].shared->sampleRate,
                targets[i].shared->channels, targets[i].shared->bits, targets[i].shared->formatTag,
                targets[i].shared->endpoint);
            if (!controls || (i == 1 && !buffers)) ok = false;
            const auto& snapshot = store.segment(ids[i])->transport.statistics;
            if (!snapshot.known) ok = false;
            uint64_t completed = 0;
            for (const auto& count : snapshot.methods) {
                if (count.unknown || count.failed) ok = false;
                completed += count.completed;
            }
            if ((i == 0 && completed) || (i == 1 && !completed)) ok = false;
            std::printf("initial_source_statistics pid=%lu completed=%llu independent_selection=%d\n",
                targets[i].pid, completed, i == 0 ? completed == 0 : completed > 0);
        }
        const uint32_t updated = wa::methodBit(wa::MonitorMethod::SetMasterVolume) |
            wa::methodBit(wa::MonitorMethod::CaptureGetBuffer) | wa::methodBit(wa::MonitorMethod::GetChannelCount);
        const auto before = store.segment(ids[1])->count;
        if (!controller.configure(instances[1], store.segment(ids[1])->token, updated)) return 45;
        if (!wait([&] { return published == 1 && store.segment(ids[1])->count > before + 5; })) return 46;
        const auto rejected = controller.configure(instances[1], store.segment(ids[0])->token, 0);
        if (!rejected) return 47;
        bool failureSeen = false;
        const auto deadline = GetTickCount64() + 5000;
        do {
            for (auto& batch : controller.poll()) {
                for (const auto& event : batch.events)
                    if (event.kind == wa::PipelineAttachController::EventKind::ConfigurationFailed) failureSeen = true;
                selection.apply(store, std::move(batch));
            }
            if (!failureSeen) Sleep(10);
        } while (!failureSeen && GetTickCount64() < deadline);
        if (!failureSeen || store.segment(ids[1])->methods != updated || controller.lastMethods(instances[1]) != updated) ok = false;
        std::printf("dynamic_selection pid=%lu mask=%u published=%u rejected_stale=%d preserved=%d\n",
            targets[1].pid, updated, published, failureSeen, controller.lastMethods(instances[1]) == updated);
    }
    if (selection.selected() != ids[0]) ok = false;
    const auto initialBackground = store.segment(ids[1])->count;
    for (int i = 0; i < 100; ++i) {
        selection.select(store, ids[static_cast<size_t>(i % 2)]);
        Sleep(10);
        consume();
        wa::call_log_filter::FilterOpts query;
        query.attach = selection.selected();
        const auto view = store.view(query);
        for (size_t row = 0; row < view.size(); ++row) {
            const auto& call = view.at(row)->call;
            if (call.pid != targets[static_cast<size_t>(i % 2)].pid ||
                call.processCreated != instances[static_cast<size_t>(i % 2)].created) ok = false;
        }
    }
    if (store.segment(ids[1])->count <= initialBackground) ok = false;
    for (size_t i = 0; i < targets.size(); ++i) {
        wa::call_log_filter::FilterOpts query;
        query.attach = ids[i];
        const auto view = store.view(query);
        size_t setters = 0;
        for (size_t row = 0; row < view.size(); ++row) {
            const auto& call = view.at(row)->call;
            if (call.method != "SetMasterVolume") continue;
            ++setters;
            if (call.args != (i ? "level=0.750" : "level=0.250") || call.hresult != S_OK) ok = false;
        }
        std::printf("segment pid=%lu created=%llu id=%llu controls=%zu setters=%zu expected_level=%.3f switches=100\n",
            targets[i].pid, instances[i].created, ids[i], view.size(), setters, i ? .75 : .25);
        if (!setters) ok = false;
    }
    selection.select(store, ids[0]);
    const auto segmentsBeforeDuplicate = store.segments(true).size();
    if (controller.start(targets[0].pid) || store.segments(true).size() != segmentsBeforeDuplicate) ok = false;
    controller.stop(targets[0].pid);
    if (!wait([&] { return !controller.attached(targets[0].pid); })) return 35;
    const auto frozenCount = store.segment(ids[0])->count;
    const auto frozenStatistics = store.segment(ids[0])->transport.statistics;
    if (selection.selected() != ids[0] || store.segment(ids[0])->state != wa::CallLogStore::SegmentState::Detached) ok = false;
    if (methods) {
        if (!controller.configure(instances[1], store.segment(ids[1])->token, initialMasks[1])) return 48;
        if (!wait([&] { return published == 2; })) return 49;
        controller.stop(targets[1].pid);
        if (!wait([&] { return !controller.attached(targets[1].pid); })) return 50;
        if (!controller.start(targets[1].pid, {}, wa::PipelineFlow::Render, instances[1])) return 51;
        if (!wait([&] { return !controller.snapshot().pendingPid; })) return 52;
        const auto newSecond = store.latestAttach(instances[1]);
        if (newSecond == ids[1] || store.segment(newSecond)->methods != initialMasks[1] ||
            !wait([&] { return store.segment(newSecond)->count > 5; })) ok = false;
        std::printf("reattach_selection pid=%lu old_segment=%llu new_segment=%llu mask=%u history_readonly=%d\n",
            targets[1].pid, ids[1], newSecond, store.segment(newSecond)->methods,
            !store.setMethods(ids[1], wa::kAllMonitorMethods));
    }
    if (!controller.start(targets[0].pid, {}, wa::PipelineFlow::Render, instances[0])) return 36;
    if (!wait([&] { return !controller.snapshot().pendingPid; })) return 37;
    const auto newest = store.latestAttach(instances[0]);
    if (newest == ids[0] || selection.selected() != ids[0] || store.initializeHintForInstance(instances[0]).hint.present) ok = false;
    if (!wait([&] { return store.segment(newest)->count > 8; })) return 38;
    if (store.segment(ids[0])->count != frozenCount) ok = false;
    for (size_t method = 0; method < frozenStatistics.methods.size(); ++method) {
        const auto& before = frozenStatistics.methods[method];
        const auto& after = store.segment(ids[0])->transport.statistics.methods[method];
        if (before.completed != after.completed || before.failed != after.failed ||
            before.discontinuities != after.discontinuities || before.unknown != after.unknown) ok = false;
    }
    InterlockedExchange(&targets[0].shared->stop, 1);
    if (!wait([&] { return !controller.attached(targets[0].pid); })) return 39;
    if (store.segment(newest)->state != wa::CallLogStore::SegmentState::Exited) ok = false;
    controller.stopAll();
    if (!wait([&] { return !controller.anyAttached(); })) return 40;
    controller.shutdown();
    if (methods) for (const auto& segment : store.segments(false)) {
        const auto& snapshot = segment.transport.statistics;
        if (!snapshot.known) ok = false;
        std::printf("segment_window pid=%u segment=%llu retained=%zu received=%llu evicted=%llu snapshot_ms=%lld\n",
            segment.instance.pid, segment.id, segment.windowRetained, segment.windowReceived,
            segment.windowEvicted, segment.transport.snapshotMs);
        for (size_t method = 0; method < snapshot.methods.size(); ++method) {
            if (wa::kMonitorMethods[method].control) continue;
            const auto& count = snapshot.methods[method];
            std::printf("source_statistics pid=%u segment=%llu method=%s::%s completed=%llu failed=%llu discontinuities=%llu unknown=%d\n",
                segment.instance.pid, segment.id, wa::kMonitorMethods[method].iface,
                wa::kMonitorMethods[method].method, count.completed, count.failed, count.discontinuities, count.unknown);
            if (count.unknown || count.failed) ok = false;
        }
    }
    for (auto& target : targets) InterlockedExchange(&target.shared->stop, 1);
    for (auto& target : targets) {
        const DWORD waited = WaitForSingleObject(target.process, 10000);
        const DWORD waitError = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        WA_LOG(waited == WAIT_OBJECT_0 ? wa::log::Level::Debug : wa::log::Level::Warn,
            "HookAudioProbe", "WaitForSingleObject/segments",
            "pid=" + std::to_string(target.pid) + " timeout_ms=10000",
            "result=" + std::to_string(waited) + " error=" + std::to_string(waitError));
        if (waited != WAIT_OBJECT_0) return 41;
        DWORD code = STILL_ACTIVE;
        if (!observeWin32("GetExitCodeProcess/segments", GetExitCodeProcess(target.process, &code) != FALSE)) return 42;
        const auto& stats = target.shared->stats[0];
        std::printf("target_exit pid=%lu code=%lu callbacks=%llu failures=%llu captured_frames=%llu\n",
            target.pid, code, stats.callbacks, stats.failures, stats.capturedFrames);
        if (code || !stats.callbacks || !stats.capturedFrames || stats.failures) ok = false;
    }
    std::printf("result=%s history=%zu selected=%llu reattached=%llu old_count=%zu coverage=unknown\n",
        ok ? "PASS" : "FAIL", store.segments(false).size(), selection.selected(), newest, frozenCount);
    return ok ? 0 : 43;
}
}

int wmain(int argc, wchar_t** argv) {
    ProbeLog logging;
    if (argc == 3 && !std::wcscmp(argv[1], L"--victim")) return victim(argv[2]);
    if (argc == 4 && !std::wcscmp(argv[1], L"--victim"))
        return victim(argv[2], static_cast<float>(std::wcstod(argv[3], nullptr)));
    if (argc == 2 && !std::wcscmp(argv[1], L"--segments")) return segments();
    if (argc == 2 && !std::wcscmp(argv[1], L"--method-selection")) return segments(true);
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
                if (!t.attach.publishMethods(t.attach.instance(), phase == 2 ? wa::kDefaultMonitorMethods : wa::kAllMonitorMethods)) return 25;
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
            if ((recording && control.empty()) || (pumping && pump.empty()) || (!pumping && pumpDelta != 0)) ok = false;
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
