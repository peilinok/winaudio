// Two real processes call production Hook entry points; this is synthetic
// transport/window evidence, not DLL injection, audio, or performance evidence.
#include "CallLogStore.h"
#include "PumpWindow.h"
#include "Log.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define DllMain SegmentWindowUnusedDllMain
#include "../hook/WinAudioHook.cpp"
#undef DllMain

namespace {
namespace ipc = wa::hook_ipc;
constexpr uint32_t kCalls = 10013, kBatch = 17, kGeneration = 1;
constexpr uint32_t kRecent = 1024;
struct SharedProbe {
    volatile LONG originals = 0;
    volatile LONG controls = 0;
    volatile LONG produced = 0;
    volatile LONG finished = 0;
    LONG warmOriginals = 0, warmControls = 0;
    uint64_t start = 0, formalStart = 0, formalEnd = 0;
};
SharedProbe* probeShared = nullptr;
uint32_t probeRow = 0;
bool probeTimed = false;

bool probeObserved(const char* call, bool success, const std::string& args,
                   wa::log::Level level = wa::log::Level::Debug) {
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    WA_LOG(success ? level : wa::log::Level::Warn, "SegmentWindowProbe", call, args,
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    return success;
}
void probeClose(HANDLE handle) {
    if (handle) probeObserved("CloseHandle", CloseHandle(handle) != FALSE,
                             "handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle)));
}
DWORD probeWait(HANDLE handle, DWORD timeout) {
    const DWORD result = WaitForSingleObject(handle, timeout);
    const DWORD error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    WA_LOG(result == WAIT_FAILED ? wa::log::Level::Warn : wa::log::Level::Trace,
           "SegmentWindowProbe", "WaitForSingleObject",
           "handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle)) +
               " timeout=" + std::to_string(timeout),
           "result=" + std::to_string(result) + " " + wa::log::hrName(HRESULT_FROM_WIN32(error)));
    return result;
}
bool probeSet(HANDLE event) {
    return probeObserved("SetEvent", SetEvent(event) != FALSE,
                         "handle=" + std::to_string(reinterpret_cast<uintptr_t>(event)));
}
HRESULT probeResult(uint32_t row) {
    return !probeTimed && row % 11 == 0 ? E_ACCESSDENIED : row % 7 == 0 ? S_FALSE : S_OK;
}
HRESULT STDMETHODCALLTYPE probeFrequency(IAudioClock*, UINT64* value) {
    InterlockedIncrement(&probeShared->originals);
    const HRESULT result = probeResult(probeRow);
    if (SUCCEEDED(result) && value) *value = 100000ULL + probeRow;
    return result;
}
HRESULT STDMETHODCALLTYPE probeRelease(IAudioRenderClient*, UINT32, DWORD) {
    InterlockedIncrement(&probeShared->originals);
    return probeResult(probeRow);
}
HRESULT STDMETHODCALLTYPE probeStart(IAudioClient*) {
    InterlockedIncrement(&probeShared->controls);
    return S_OK;
}
HRESULT probeInvoke(unsigned index, uint32_t row) {
    probeRow = row;
    if (index == 0) {
        UINT64 frequency = 0;
        return HookClockFreq(reinterpret_cast<IAudioClock*>(static_cast<uintptr_t>(0x1000)), &frequency);
    }
    return HookRenReleaseBuffer(reinterpret_cast<IAudioRenderClient*>(static_cast<uintptr_t>(0x2000)),
                                row, AUDCLNT_BUFFERFLAGS_SILENT);
}
wa::MonitorMethod probeMethod(unsigned index) {
    return index == 0 ? wa::MonitorMethod::GetFrequency : wa::MonitorMethod::RenderReleaseBuffer;
}

struct ProbeHandles {
    HANDLE mapping = nullptr, ready = nullptr, resume = nullptr, ringMapping = nullptr;
    SharedProbe* shared = nullptr;
    ipc::Ring* ring = nullptr;
    ~ProbeHandles() {
        if (ring) probeObserved("UnmapViewOfFile/ring", UnmapViewOfFile(ring) != FALSE, "ring");
        if (shared) probeObserved("UnmapViewOfFile/shared", UnmapViewOfFile(shared) != FALSE, "probe control");
        probeClose(ringMapping);
        probeClose(resume);
        probeClose(ready);
        probeClose(mapping);
    }
};

int probeChild(const wchar_t* name, unsigned index, unsigned rate = 0,
               unsigned warmup = 0, unsigned seconds = 0, bool controls = true) {
    ProbeHandles handles;
    handles.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!probeObserved("OpenFileMappingW/control", handles.mapping != nullptr, "child")) return 10;
    handles.shared = static_cast<SharedProbe*>(MapViewOfFile(handles.mapping, FILE_MAP_ALL_ACCESS,
                                                            0, 0, sizeof(SharedProbe)));
    if (!probeObserved("MapViewOfFile/control", handles.shared != nullptr, "child")) return 11;
    const std::wstring readyName = std::wstring(name) + L"-ready";
    const std::wstring resumeName = std::wstring(name) + L"-resume";
    handles.ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    if (!probeObserved("OpenEventW/ready", handles.ready != nullptr, "child")) return 12;
    handles.resume = OpenEventW(SYNCHRONIZE, FALSE, resumeName.c_str());
    if (!probeObserved("OpenEventW/resume", handles.resume != nullptr, "child")) return 13;
    wchar_t ringName[64]{};
    ipc::mapName(GetCurrentProcessId(), ringName, std::size(ringName));
    handles.ringMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ringName);
    if (!probeObserved("OpenFileMappingW/ring", handles.ringMapping != nullptr, "child")) return 14;
    handles.ring = static_cast<ipc::Ring*>(MapViewOfFile(handles.ringMapping, FILE_MAP_ALL_ACCESS,
                                                       0, 0, sizeof(ipc::Ring)));
    if (!probeObserved("MapViewOfFile/ring", handles.ring != nullptr, "child")) return 15;
    if (!ipc::compatibleRing(*handles.ring)) return 16;
    g_ring = handles.ring;
    probeShared = handles.shared;
    g_clockFreq = &probeFrequency;
    g_renRel = &probeRelease;
    g_start = &probeStart;
    if (rate) {
        probeTimed = true;
        if (!probeSet(handles.ready) || probeWait(handles.resume, 10000) != WAIT_OBJECT_0) return 21;
        LARGE_INTEGER frequency{}, now{};
        QueryPerformanceFrequency(&frequency);
        const auto tick = [&] { QueryPerformanceCounter(&now); return static_cast<uint64_t>(now.QuadPart); };
        const auto start = probeShared->start;
        const auto formal = start + warmup * static_cast<uint64_t>(frequency.QuadPart);
        const auto end = formal + seconds * static_cast<uint64_t>(frequency.QuadPart);
        uint64_t issued = 0, issuedControls = 0;
        bool formalStarted = false;
        while (tick() < end) {
            const auto current = static_cast<uint64_t>(now.QuadPart);
            if (current < start) { Sleep(1); continue; }
            if (!formalStarted && current >= formal) {
                probeShared->warmOriginals = ReadAcquire(&probeShared->originals);
                probeShared->warmControls = ReadAcquire(&probeShared->controls);
                probeShared->formalStart = formal;
                formalStarted = true;
            }
            const auto elapsed = current - start;
            const auto wanted = elapsed * rate / static_cast<uint64_t>(frequency.QuadPart);
            // Catch up in bounded bursts; originals never wait for the receiver.
            for (unsigned burst = 0; issued < wanted && burst < 256; ++burst) {
                ++issued;
                if (probeInvoke(index, static_cast<uint32_t>(issued)) != probeResult(static_cast<uint32_t>(issued))) return 22;
            }
            const auto wantedControls = elapsed / static_cast<uint64_t>(frequency.QuadPart) + 1;
            if (controls && issuedControls < wantedControls) {
                if (HookStart(reinterpret_cast<IAudioClient*>(static_cast<uintptr_t>(0x3000))) != S_OK) return 23;
                ++issuedControls;
            }
            if (issued >= wanted) Sleep(1);
        }
        probeShared->formalEnd = tick();
        InterlockedExchange(&probeShared->finished, 1);
        if (!probeSet(handles.ready) || probeWait(handles.resume, 10000) != WAIT_OBJECT_0) return 24;
        g_ring = nullptr;
        probeShared = nullptr;
        return 0;
    }
    if (HookStart(reinterpret_cast<IAudioClient*>(static_cast<uintptr_t>(0x3000))) != S_OK) return 17;
    const auto enabled = wa::methodBit(probeMethod(index)) | wa::methodBit(wa::MonitorMethod::Start);
    for (uint32_t row = 1; row <= kCalls; ++row) {
        if (row == kCalls / 2 + 1) {
            ipc::publishMonitorState(*g_ring, wa::methodBit(wa::MonitorMethod::Start), kGeneration);
            if (probeInvoke(index, 20001) != probeResult(20001) ||
                probeInvoke(index, 20002) != probeResult(20002)) return 18;
            ipc::publishMonitorState(*g_ring, enabled, kGeneration);
        }
        if (probeInvoke(index, row) != probeResult(row)) return 19;
        if (row % kBatch == 0) {
            InterlockedExchange(&probeShared->produced, static_cast<LONG>(row));
            if (!probeSet(handles.ready) || probeWait(handles.resume, 10000) != WAIT_OBJECT_0) return 20;
        }
    }
    g_ring = nullptr;
    probeShared = nullptr;
    return 0;
}

struct ProbeTarget {
    ProbeHandles handles;
    PROCESS_INFORMATION process{};
    wa::ProcessInstance instance;
    wa::PumpWindow window;
    wa::HookTransportReader controls;
    wa::HookTransportLaneStatus transport;
    wa::MethodStatisticsSnapshot statistics;
    uint64_t attach = 0, received = 0, checkedWindow = 0;
    uint64_t invalid = 0, controlReceived = 0;
    DWORD exitCode = STILL_ACTIVE;
    bool exited = false, frozen = false, freshEmpty = false, zeroStatistics = false;
    ~ProbeTarget() {
        if (process.hProcess && !exited) {
            probeObserved("TerminateProcess/owned child", TerminateProcess(process.hProcess, 91) != FALSE,
                           "pid=" + std::to_string(process.dwProcessId), wa::log::Level::Warn);
            probeWait(process.hProcess, 5000);
        }
        probeClose(process.hThread);
        probeClose(process.hProcess);
    }
};

bool probeLaunch(ProbeTarget& target, unsigned index, const wchar_t* self, wa::CallLogStore& store,
                 unsigned rate = 0, unsigned warmup = 0, unsigned seconds = 0, bool controls = true) {
    const std::wstring name = L"Local\\WinAudioSegmentProbe-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(index);
    target.handles.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                               0, sizeof(SharedProbe), name.c_str());
    if (!probeObserved("CreateFileMappingW/control", target.handles.mapping != nullptr, "parent")) return false;
    target.handles.shared = static_cast<SharedProbe*>(MapViewOfFile(target.handles.mapping,
        FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedProbe)));
    if (!probeObserved("MapViewOfFile/control", target.handles.shared != nullptr, "parent")) return false;
    ZeroMemory(target.handles.shared, sizeof(SharedProbe));
    const std::wstring readyName = name + L"-ready", resumeName = name + L"-resume";
    target.handles.ready = CreateEventW(nullptr, FALSE, FALSE, readyName.c_str());
    if (!probeObserved("CreateEventW/ready", target.handles.ready != nullptr, "parent")) return false;
    target.handles.resume = CreateEventW(nullptr, FALSE, FALSE, resumeName.c_str());
    if (!probeObserved("CreateEventW/resume", target.handles.resume != nullptr, "parent")) return false;
    std::wstring command = L"\"" + std::wstring(self) + L"\" --child \"" + name + L"\" " + std::to_wstring(index);
    if (rate) command += L" " + std::to_wstring(rate) + L" " + std::to_wstring(warmup) + L" " +
        std::to_wstring(seconds) + L" " + std::to_wstring(controls);
    STARTUPINFOW startup{sizeof(startup)};
    if (!probeObserved("CreateProcessW", CreateProcessW(self, command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &target.process) != FALSE,
            "controlled self --child", wa::log::Level::Info)) return false;
    const auto identity = wa::queryProcessInstance(target.process.dwProcessId, target.instance);
    if (!identity) return false;
    target.handles.ringMapping = ipc::createHookMapping(target.instance.pid);
    if (!probeObserved("createHookMapping", target.handles.ringMapping != nullptr,
                       "pid=" + std::to_string(target.instance.pid))) return false;
    target.handles.ring = static_cast<ipc::Ring*>(MapViewOfFile(target.handles.ringMapping,
        FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::Ring)));
    if (!probeObserved("MapViewOfFile/ring", target.handles.ring != nullptr, "parent")) return false;
    auto& ring = *target.handles.ring;
    ipc::initializeRing(ring);
    ring.processCreated = target.instance.created;
    const auto mask = wa::methodBit(probeMethod(index)) | wa::methodBit(wa::MonitorMethod::Start);
    ipc::publishMonitorState(ring, mask, kGeneration);
    ring.captureEnabled = ring.pumpEnabled = 1;
    target.window.begin(ring);
    target.controls.begin(ring, false, kGeneration);
    target.attach = store.registerAttach(target.instance, index ? "render-child" : "clock-child", index + 1, 1);
    store.setObservationGeneration(target.attach, kGeneration);
    target.zeroStatistics = ipc::readMethodStatistics(ring, kGeneration).known;
    const auto initial = ipc::readMethodStatistics(ring, kGeneration);
    for (const auto& item : initial.methods)
        target.zeroStatistics = !item.completed && !item.failed && !item.discontinuities &&
            !item.unknown && target.zeroStatistics;
    const DWORD previous = ResumeThread(target.process.hThread);
    return probeObserved("ResumeThread", previous != static_cast<DWORD>(-1), "parent") &&
        previous == 1 && target.attach != 0 && target.zeroStatistics;
}

bool probeValidate(const wa::HookedCall& call, unsigned index, uint32_t row) {
    if (call.hresult != probeResult(row) || !call.pump) return false;
    if (index == 0)
        return call.iface == "IAudioClock" && call.method == "GetFrequency" &&
            call.streamId == 0x1000 && call.args ==
                (FAILED(probeResult(row)) ? "" : "hz=" + std::to_string(100000ULL + row));
    return call.iface == "IAudioRenderClient" && call.method == "ReleaseBuffer" &&
        call.streamId == 0x2000 && call.args == "frames=" + std::to_string(row) + " flags=0x2";
}

bool probeReceive(ProbeTarget& target, unsigned index, uint32_t batch, wa::CallLogStore& store) {
    if (probeWait(target.handles.ready, 10000) != WAIT_OBJECT_0) return false;
    if (ReadAcquire(&target.handles.shared->produced) != static_cast<LONG>(batch * kBatch)) return false;
    target.window.poll(*target.handles.ring);
    auto received = target.window.takeReceived();
    if (received.size() != kBatch) return false;
    for (const auto& item : received) {
        ++target.received;
        if (item.sequence != target.received ||
            !probeValidate(item.call, index, static_cast<uint32_t>(target.received))) ++target.invalid;
    }
    store.appendPump(target.attach, received);
    target.controls.poll(*target.handles.ring, [&](const ipc::CallPod& pod, uint64_t) {
        ++target.controlReceived;
        if (std::string(pod.iface) != "IAudioClient" || std::string(pod.method) != "Start" || pod.hresult != S_OK)
            ++target.invalid;
        store.append(target.attach, wa::decodeHookedCallPod(pod));
    });
    if (batch == kCalls / kBatch) return true;
    return probeSet(target.handles.resume);
}

bool probeFinish(ProbeTarget& target, unsigned index, wa::CallLogStore& store) {
    auto& ring = *target.handles.ring;
    target.statistics = ipc::readMethodStatistics(ring, kGeneration);
    target.transport = target.window.transportStatus(ring);
    bool ok = target.statistics.known && !target.transport.unknown && !target.transport.pending &&
        !target.transport.overwritten && !target.transport.writeRejected &&
        target.received == kCalls && !target.invalid && target.controlReceived == 1 && store.size() == 2 &&
        ReadAcquire(&target.handles.shared->originals) == static_cast<LONG>(kCalls + 2) &&
        ReadAcquire(&target.handles.shared->controls) == 1;
    for (unsigned method = 0; method < static_cast<unsigned>(wa::MonitorMethod::Count); ++method) {
        const auto& count = target.statistics.methods[method];
        const bool chosen = method == static_cast<unsigned>(probeMethod(index));
        ok = count.completed == (chosen ? kCalls : 0) && count.failed == (chosen ? kCalls / 11 : 0) &&
            !count.discontinuities && !count.unknown && ok;
    }
    wa::call_log_filter::FilterOpts query;
    query.attach = target.attach;
    const auto view = store.view(query);
    ok = view.size() == kRecent + 1 && ok;
    for (size_t row = 0; row < view.size(); ++row) {
        const auto* record = view.at(row);
        if (!record) { ok = false; continue; }
        if (record->id.lane == wa::CallLane::Control) continue;
        const auto expected = kCalls - kRecent + static_cast<uint32_t>(target.checkedWindow) + 1;
        if (record->id.attach != target.attach || record->call.pid != target.instance.pid ||
            record->id.sequence != expected || !probeValidate(record->call, index, expected)) ok = false;
        ++target.checkedWindow;
    }
    ok = target.checkedWindow == kRecent && ok;
    wa::HookTransportStatus status;
    status.snapshotMs = 100;
    status.snapshotSequence = 1;
    status.observationGeneration = kGeneration;
    status.pump = target.transport;
    status.statistics = target.statistics;
    ok = store.updateTransport({target.instance, index + 1, status}) && ok;
    const auto before = store.segment(target.attach)->transport.statistics;
    store.endAttach(target.attach, false, 100);
    // A rejected late received row cannot refill an archived segment.
    wa::HookedCall late;
    late.iface = "LateOldGeneration";
    store.appendPump(target.attach, {{kCalls + 1, late}});
    status.snapshotSequence = 2;
    target.frozen = !store.updateTransport({target.instance, index + 1, status}) &&
        store.segment(target.attach)->transport.statistics.methods[static_cast<unsigned>(probeMethod(index))].completed ==
            before.methods[static_cast<unsigned>(probeMethod(index))].completed;
    query.attach = target.attach;
    target.frozen = store.view(query).size() == kRecent + 1 && target.frozen;
    const auto fresh = store.registerAttach(target.instance, "reattached-empty", index + 3, 101);
    query.attach = fresh;
    target.freshEmpty = fresh != target.attach && store.view(query).size() == 0;
    ipc::publishMonitorState(ring, 0, kGeneration + 1);
    const auto freshStats = ipc::readMethodStatistics(ring, kGeneration + 1);
    target.freshEmpty = freshStats.known && target.freshEmpty;
    for (const auto& method : freshStats.methods)
        target.freshEmpty = !method.completed && !method.failed && !method.discontinuities &&
            !method.unknown && target.freshEmpty;
    ok = target.frozen && target.freshEmpty && target.zeroStatistics && probeSet(target.handles.resume) && ok;
    if (probeWait(target.process.hProcess, 10000) != WAIT_OBJECT_0) return false;
    target.exited = true;
    ok = probeObserved("GetExitCodeProcess", GetExitCodeProcess(target.process.hProcess, &target.exitCode) != FALSE,
                       "pid=" + std::to_string(target.instance.pid)) && target.exitCode == 0 && ok;
    return ok;
}

int probeParent(const std::filesystem::path& output) {
    if (!output.is_absolute()) return 30;
    wchar_t self[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (!probeObserved("GetModuleFileNameW", length != 0 && length < std::size(self), "self")) return 31;
    wa::CallLogStore store;
    store.setPumpVisible(true);
    std::array<ProbeTarget, 2> targets;
    bool ok = probeLaunch(targets[0], 0, self, store) && probeLaunch(targets[1], 1, self, store);
    for (uint32_t batch = 1; ok && batch <= kCalls / kBatch; ++batch)
        for (unsigned index = 0; ok && index < targets.size(); ++index)
            ok = probeReceive(targets[index], index, batch, store);
    if (ok) for (unsigned index = 0; index < targets.size(); ++index)
        ok = probeFinish(targets[index], index, store) && ok;
    std::ofstream out(output, std::ios::trunc);
    if (!out) return 32;
    out << std::boolalpha << "{\"success\":" << ok << ",\"synthetic_only\":true,\"real_processes\":true,"
        << "\"production_hook_entry_points\":true,\"real_injection\":false,\"real_audio\":false,"
        << "\"performance_evidence\":false,\"layout\":" << ipc::kRingLayout
        << ",\"ring_bytes\":" << sizeof(ipc::Ring) << ",\"call_pod_bytes\":" << sizeof(ipc::CallPod)
        << ",\"pointer_bits\":" << sizeof(void*) * 8 << ",\"generation\":" << kGeneration
        << ",\"transport_capacity\":" << ipc::kPumpCap << ",\"host_window_capacity\":" << kRecent
        << ",\"batch_size\":" << kBatch << ",\"processes\":[";
    for (unsigned index = 0; index < targets.size(); ++index) {
        const auto& target = targets[index];
        const auto& count = target.statistics.methods[static_cast<unsigned>(probeMethod(index))];
        out << (index ? "," : "") << "{\"pid\":" << target.instance.pid
            << ",\"created_filetime\":" << target.instance.created
            << ",\"method\":\"" << (index ? "IAudioRenderClient::ReleaseBuffer" : "IAudioClock::GetFrequency")
            << "\",\"received\":" << target.received << ",\"window_rows_checked\":" << target.checkedWindow
            << ",\"window_first_sequence\":" << kCalls - kRecent + 1 << ",\"window_last_sequence\":" << kCalls
            << ",\"control_received\":" << target.controlReceived << ",\"invalid\":" << target.invalid
            << ",\"source_completed\":" << count.completed << ",\"source_failed\":" << count.failed
            << ",\"source_discontinuities\":" << count.discontinuities << ",\"source_unknown\":" << count.unknown
            << ",\"source_snapshot_known\":" << target.statistics.known
            << ",\"original_calls\":" << (target.handles.shared ? ReadAcquire(&target.handles.shared->originals) : 0)
            << ",\"overwritten\":" << target.transport.overwritten
            << ",\"write_rejected\":" << target.transport.writeRejected
            << ",\"transport_unknown\":" << target.transport.unknown
            << ",\"initial_statistics_zero\":" << target.zeroStatistics
            << ",\"history_frozen\":" << target.frozen << ",\"new_segment_empty\":" << target.freshEmpty
            << ",\"exited\":" << target.exited << ",\"exit_code\":" << target.exitCode << "}";
    }
    out << "]}\n";
    out.close();
    return ok && out ? 0 : 33;
}
}  // namespace

#include "SegmentWindowTiming.h"

int wmain(int argc, wchar_t** argv) {
    wa::log::init();
    wa::log::addStderrSink();
    int result = 34;
    if (argc == 4 && !std::wcscmp(argv[1], L"--child"))
        result = probeChild(argv[2], static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10)));
    else if (argc == 8 && !std::wcscmp(argv[1], L"--child"))
        result = probeChild(argv[2], static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10)),
            static_cast<unsigned>(std::wcstoul(argv[4], nullptr, 10)),
            static_cast<unsigned>(std::wcstoul(argv[5], nullptr, 10)),
            static_cast<unsigned>(std::wcstoul(argv[6], nullptr, 10)), std::wcstoul(argv[7], nullptr, 10) != 0);
    else if (argc == 7 && !std::wcscmp(argv[1], L"--timed"))
        result = timedParent(argv[2], argv[3], static_cast<unsigned>(std::wcstoul(argv[4], nullptr, 10)),
            static_cast<unsigned>(std::wcstoul(argv[5], nullptr, 10)), std::wcstoul(argv[6], nullptr, 10) != 0);
    else if (argc == 3 && !std::wcscmp(argv[1], L"--output")) result = probeParent(argv[2]);
    wa::log::shutdown();
    return result;
}
