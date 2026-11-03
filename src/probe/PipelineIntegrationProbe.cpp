// Reuse the existing real-audio fixture without changing its performance image.
#define wmain hookAudioProbeMain
#include "HookAudioProbe.cpp"
#undef wmain
#include <chrono>

namespace {
struct IntegrationStartupMarkers {
    uint32_t pid = 0, tlsEntered = 0, crtEntered = 0, mainEntered = 0;
    int64_t tlsCounter = 0, crtCounter = 0, mainCounter = 0;
};
void integrationStartupMarker(unsigned stage) {
    const wchar_t* token = wcsstr(GetCommandLineW(), L"--startup-gate=");
    if (!token) return;
    token += 15;
    wchar_t name[180]{};
    unsigned size = 0;
    while (token[size] && token[size] != L' ' && size < 150) { name[size] = token[size]; ++size; }
    wcscat_s(name, L"-map");
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    auto* marker = map ? static_cast<IntegrationStartupMarkers*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
    if (marker) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        marker->pid = GetCurrentProcessId();
        if (stage == 0) { marker->tlsCounter = counter.QuadPart; marker->tlsEntered = 1; }
        else if (stage == 1) { marker->crtCounter = counter.QuadPart; marker->crtEntered = 1; }
        else { marker->mainCounter = counter.QuadPart; marker->mainEntered = 1; }
        UnmapViewOfFile(marker);
    }
    if (map) CloseHandle(map);
}
// Test-only loader gate. No audio or C++ initialization runs under loader lock.
void NTAPI integrationTls(void*, DWORD reason, void*) {
    if (reason != DLL_PROCESS_ATTACH) return;
    const wchar_t* token = wcsstr(GetCommandLineW(), L"--startup-gate=");
    if (!token) return;
    token += 15;
    wchar_t name[160]{};
    unsigned size = 0;
    while (token[size] && token[size] != L' ' && size < 150) { name[size] = token[size]; ++size; }
    integrationStartupMarker(0);
    SetEnvironmentVariableW(L"WA_REMOTE_GATE", name);
    LoadLibraryW(L"RemoteInjectionGate.dll");
}
#ifdef _WIN64
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:integrationStartupTls")
#else
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_integrationStartupTls")
#endif
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK integrationStartupTls = integrationTls;
#pragma const_seg()
struct IntegrationCrtMarker { IntegrationCrtMarker() { integrationStartupMarker(1); } } integrationCrtMarker;

int64_t integrationCounter() {
    LARGE_INTEGER counter{};
    observeWin32("QueryPerformanceCounter/integration", QueryPerformanceCounter(&counter) != FALSE);
    return counter.QuadPart;
}

DWORD integrationWait(const char* operation, HANDLE handle, DWORD timeout) {
    const DWORD outcome = WaitForSingleObject(handle, timeout);
    const DWORD error = outcome == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    const char* state = outcome == WAIT_OBJECT_0 ? "WAIT_OBJECT_0/signaled" :
        outcome == WAIT_TIMEOUT ? "WAIT_TIMEOUT" : outcome == WAIT_FAILED ? "WAIT_FAILED" : "other";
    const HRESULT hr = outcome == WAIT_FAILED ? HRESULT_FROM_WIN32(error) :
        outcome == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : S_OK;
    WA_LOG(FAILED(hr) ? wa::log::Level::Warn : wa::log::Level::Debug, "PipelineIntegrationProbe", operation,
        "handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle)) + " timeout=" + std::to_string(timeout),
        std::string(state) + " result=" + std::to_string(outcome) + " error=" + std::to_string(error) + " " + wa::log::hrName(hr));
    return outcome;
}

struct IntegrationGate {
    std::wstring token;
    HANDLE map = nullptr, gate = nullptr, entered = nullptr, ready = nullptr, stop = nullptr, exit = nullptr;
    PROCESS_INFORMATION process{};
    IntegrationStartupMarkers* markers = nullptr;
    explicit IntegrationGate(unsigned index) {
        token = L"Local\\WinAudio-Integration-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(index);
        map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(IntegrationStartupMarkers), (token + L"-map").c_str());
        observeWin32("CreateFileMappingW/integration-marker", map != nullptr);
        if (map) markers = static_cast<IntegrationStartupMarkers*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        observeWin32("MapViewOfFile/integration-marker", markers != nullptr);
        gate = CreateEventW(nullptr, TRUE, FALSE, (token + L"-gate").c_str());
        observeWin32("CreateEventW/integration-gate", gate != nullptr);
        entered = CreateEventW(nullptr, TRUE, FALSE, (token + L"-entered").c_str());
        observeWin32("CreateEventW/integration-entered", entered != nullptr);
        ready = CreateEventW(nullptr, TRUE, FALSE, (token + L"-ready").c_str());
        observeWin32("CreateEventW/integration-ready", ready != nullptr);
        stop = CreateEventW(nullptr, TRUE, FALSE, (token + L"-stop").c_str());
        observeWin32("CreateEventW/integration-stop", stop != nullptr);
        exit = CreateEventW(nullptr, TRUE, FALSE, (token + L"-exit").c_str());
        observeWin32("CreateEventW/integration-exit", exit != nullptr);
        observe("CreateFileMappingW/CreateEventW/integration", valid() ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    }
    bool valid() const { return map && markers && gate && entered && ready && stop && exit; }
    bool spawn(const std::filesystem::path& folder, bool held) {
        const auto executable = folder / L"RemoteInjectionTarget.exe";
        auto command = L"\"" + executable.wstring() + L"\" " + token + (held ? L" --hold-loader" : L" --exit-on-go");
        STARTUPINFOW startup{sizeof(startup)};
        if (!observeWin32("CreateProcessW/integration-gate", CreateProcessW(executable.c_str(), command.data(),
            nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &process) != FALSE)) return false;
        return integrationWait("WaitForSingleObject/integration-ready", ready, 5000) == WAIT_OBJECT_0 &&
            (!held || integrationWait("WaitForSingleObject/integration-entered", entered, 5000) == WAIT_OBJECT_0);
    }
    bool finish() {
        if (!process.hProcess) return true;
        observeWin32("SetEvent/gate", SetEvent(gate) != FALSE);
        observeWin32("SetEvent/stop", SetEvent(stop) != FALSE);
        observeWin32("SetEvent/exit", SetEvent(exit) != FALSE);
        const DWORD waited = integrationWait("WaitForSingleObject/integration-gate", process.hProcess, 10000);
        DWORD code = STILL_ACTIVE;
        const bool queried = observeWin32("GetExitCodeProcess/integration-gate",
            GetExitCodeProcess(process.hProcess, &code) != FALSE);
        std::printf("gate_exit pid=%lu waited=%lu code=%lu\n", process.dwProcessId, waited, code);
        return waited == WAIT_OBJECT_0 && queried && code == 0;
    }
    ~IntegrationGate() {
        finish();
        if (markers) observeWin32("UnmapViewOfFile/integration-marker", UnmapViewOfFile(markers) != FALSE);
        if (process.hProcess) observeWin32("CloseHandle/integration-process", CloseHandle(process.hProcess) != FALSE);
        if (process.hThread) observeWin32("CloseHandle/integration-thread", CloseHandle(process.hThread) != FALSE);
        for (HANDLE handle : {map, gate, entered, ready, stop, exit})
            if (handle) observeWin32("CloseHandle/integration-event", CloseHandle(handle) != FALSE);
    }
};

std::string frozenText(const wa::CallLogExport& snapshot) {
    std::string result;
    for (const auto& line : wa::CallLogStore::exportSummary(snapshot, false)) result += line + "\n";
    for (const auto& row : snapshot.records) result += wa::CallLogStore::toJsonLine(*row) + "\n";
    return result;
}

int integration(const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    LARGE_INTEGER frequency{};
    observeWin32("QueryPerformanceFrequency/integration", QueryPerformanceFrequency(&frequency) != FALSE);
    std::printf("integration_clock frequency=%lld\n", frequency.QuadPart);
    wchar_t self[MAX_PATH]{};
    observeWin32("GetModuleFileNameW/integration", GetModuleFileNameW(nullptr, self, MAX_PATH) != 0);
    const auto folder = std::filesystem::path(self).parent_path();
    IntegrationGate startupGate(0), loader(1), exiting(2);
    if (!startupGate.valid() || !loader.valid() || !exiting.valid()) return 60;
    std::array<Target, 4> targets; // A, C, D, then startup B.
    for (size_t index = 0; index < 3; ++index)
        if (!launch(targets[index], index, 4)) return 61;
    if (!loader.spawn(folder, true) || !exiting.spawn(folder, false)) return 62;
    auto& b = targets[3];
    const auto mappingName = startupGate.token + L"-audio";
    b.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), mappingName.c_str());
    if (!observeWin32("CreateFileMappingW/startup-audio", b.map != nullptr)) return 63;
    b.shared = static_cast<Shared*>(MapViewOfFile(b.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!observeWin32("MapViewOfFile/startup-audio", b.shared != nullptr)) return 64;
    b.shared->controlEvery = 4;
    wa::PipelineAttachController controller;
    wa::CallLogStore store;
    wa::pipeline_attach_ui::SegmentSelection selection;
    store.setPumpVisible(true);
    bool ok = true;
    unsigned configured = 0, cleared = 0;
    const auto receive = [&] {
        const auto apply = [&](const auto& event) {
            std::printf("event qpc=%lld pid=%u created=%llu token=%llu kind=%u methods=%u generation=%u hr=%ld\n",
                integrationCounter(), event.pid, event.instance.created, event.token, static_cast<unsigned>(event.kind),
                event.methods, event.observationGeneration, event.result.code);
            if (event.kind == wa::PipelineAttachController::EventKind::Configured) ++configured;
            if (event.kind == wa::PipelineAttachController::EventKind::Cleared) ++cleared;
        };
        for (auto& event : controller.pollControlFeedback()) {
            apply(event);
            wa::PipelineAttachController::Batch feedback;
            feedback.events.push_back(std::move(event));
            selection.apply(store, std::move(feedback));
        }
        for (auto& batch : controller.poll()) {
            for (const auto& event : batch.events) apply(event);
            selection.apply(store, std::move(batch));
        }
    };
    const auto wait = [&](const auto& complete, uint32_t timeout = 15000) {
        const auto deadline = GetTickCount64() + timeout;
        do { receive(); if (complete()) return true; Sleep(5); } while (GetTickCount64() < deadline);
        return false;
    };
    std::array<wa::ProcessInstance, 4> instances{};
    std::array<uint64_t, 4> ids{};
    for (size_t index = 0; index < 3; ++index)
        if (!wa::queryProcessInstance(targets[index].pid, instances[index])) return 65;
    const uint32_t aMethods = wa::kDefaultMonitorMethods | wa::methodBit(wa::MonitorMethod::CaptureGetBuffer);
    if (!controller.start(targets[0].pid, {}, wa::PipelineFlow::Render, instances[0], aMethods) ||
        !wait([&] { return store.activeAttach(targets[0].pid) != 0; })) return 66;
    ids[0] = selection.selected();
    const int64_t aAttached = integrationCounter();
    const auto aToken = store.segment(ids[0])->token;
    wa::StartupRequest request;
    request.executable = self;
    request.arguments = L"--victim " + mappingName + L" --startup-gate=" + startupGate.token;
    request.directory = folder.wstring();
    request.timeoutMs = 90000; // Public fixture timeout; the GUI default remains 15 seconds.
    if (!controller.startStartup(request, wa::kAllMonitorMethods) ||
        !wait([&] { return WaitForSingleObject(startupGate.entered, 0) == WAIT_OBJECT_0; })) return 67;
    auto startup = controller.startupStatus();
    b.pid = startup.outcome.pid;
    b.process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, b.pid);
    if (!observeWin32("OpenProcess/startup-audio", b.process != nullptr) || !wa::queryProcessInstance(b.pid, instances[3])) return 68;
    const int64_t startupHeld = integrationCounter();
    wa::ProcessInstance loaderInstance, exitingInstance, selfInstance;
    if (!wa::queryProcessInstance(loader.process.dwProcessId, loaderInstance) ||
        !wa::queryProcessInstance(exiting.process.dwProcessId, exitingInstance) ||
        !wa::queryProcessInstance(GetCurrentProcessId(), selfInstance)) return 69;
    const std::vector<wa::PipelineAttachController::Target> requests{
        {instances[1], {}, wa::PipelineFlow::Render, wa::kAllMonitorMethods},
        {instances[2], {}, wa::PipelineFlow::Render, wa::kAllMonitorMethods},
        {loaderInstance}, {exitingInstance}, {selfInstance}, {instances[0]}, {instances[1]}};
    const int64_t batchStarted = integrationCounter();
    if (!controller.startBatch(requests) || !wait([&] {
        const auto status = controller.batchStatus();
        return status.items.size() == 6 && status.items[0].state == wa::PipelineAttachController::ItemState::Succeeded &&
            status.items[1].state == wa::PipelineAttachController::ItemState::Succeeded &&
            controller.snapshot().pendingPid == loader.process.dwProcessId;
    })) return 70;
    const int64_t duringCounter = integrationCounter();
    const auto duringReceived = store.segment(ids[0])->windowReceived;
    const auto duringCalls = targets[0].shared->stats[0].originalCalls;
    std::printf("overlap_baseline qpc=%lld a_received=%llu a_source_calls=%llu b_running=%d batch_running=%d "
        "pending_pid=%u b_tls=%u b_crt=%u b_main=%u\n", duringCounter, duringReceived, duringCalls,
        controller.startupStatus().running, controller.batchStatus().running, controller.snapshot().pendingPid,
        startupGate.markers->tlsEntered, startupGate.markers->crtEntered, startupGate.markers->mainEntered);
    observeWin32("SetEvent/queued-exit", SetEvent(exiting.exit) != FALSE);
    if (integrationWait("WaitForSingleObject/queued-exit", exiting.process.hProcess, 5000) != WAIT_OBJECT_0) return 71;
    const uint32_t updatedMethods = aMethods | wa::methodBit(wa::MonitorMethod::RenderGetBuffer);
    if (!controller.configure(instances[0], aToken, updatedMethods) || !wait([&] {
        return configured == 1 && store.segment(ids[0])->windowReceived > duringReceived + 50;
    })) return 72;
    const int64_t progressed = integrationCounter();
    const auto& marker = *startupGate.markers;
    const bool overlapped = startupHeld <= batchStarted && batchStarted < progressed &&
        controller.startupStatus().running && controller.batchStatus().running &&
        WaitForSingleObject(startupGate.gate, 0) == WAIT_TIMEOUT && b.shared->ready == 0 &&
        marker.pid == b.pid && marker.tlsEntered == 1 && marker.crtEntered == 0 && marker.mainEntered == 0 &&
        targets[0].shared->stats[0].originalCalls > duringCalls && duringCounter < progressed && selection.selected() == ids[0];
    std::printf("overlap startup_held=%lld batch_started=%lld progressed=%lld startup_pid=%lu stage=%s startup_ready=%ld "
        "a_received_before=%llu a_received_after=%llu a_calls_before=%llu a_calls_after=%llu configured=%u "
        "batch_pending=%u actual_intersection=%d\n", startupHeld, batchStarted, progressed, b.pid,
        wa::startupStageName(controller.startupStatus().stage), b.shared->ready, duringReceived,
        store.segment(ids[0])->windowReceived, duringCalls, targets[0].shared->stats[0].originalCalls,
        configured, controller.snapshot().pendingPid, overlapped);
    ok = overlapped && ok;
    std::printf("startup_marker held_qpc=%lld tls=%u crt=%u main=%u tls_qpc=%lld independent_entered=%d committed=%d\n",
        progressed, marker.tlsEntered, marker.crtEntered, marker.mainEntered, marker.tlsCounter,
        WaitForSingleObject(startupGate.entered, 0) == WAIT_OBJECT_0, controller.startupStatus().outcome.committed);
    if (!controller.stop(targets[0].pid) || !wait([&] {
        return store.segment(ids[0])->state == wa::CallLogStore::SegmentState::Detached;
    })) return 73;
    const int64_t aDetached = integrationCounter();
    std::printf("detach_during_overlap qpc=%lld a_received=%llu a_source_calls=%llu b_running=%d batch_running=%d b_crt=%u b_main=%u\n",
        aDetached, store.segment(ids[0])->windowReceived, targets[0].shared->stats[0].originalCalls,
        controller.startupStatus().running, controller.batchStatus().running, marker.crtEntered, marker.mainEntered);
    ok = controller.startupStatus().running && controller.batchStatus().running && selection.selected() == ids[0] && ok;
    if (!wait([&] { return !controller.batchStatus().running; }, 20000)) return 74;
    const int64_t batchEnded = integrationCounter();
    const auto status = controller.batchStatus();
    using State = wa::PipelineAttachController::ItemState;
    const std::array<State, 6> expected{State::Succeeded, State::Succeeded, State::CleanupPending,
        State::TargetChanged, State::Rejected, State::Cancelled};
    for (size_t index = 0; index < status.items.size(); ++index) {
        const auto& item = status.items[index];
        std::printf("batch_item index=%zu pid=%u created=%llu state=%u expected=%u hr=%ld message=%s\n", index,
            item.target.instance.pid, item.target.instance.created, static_cast<unsigned>(item.state),
            static_cast<unsigned>(expected[index]), item.result.code, item.result.message.c_str());
        ok = item.state == expected[index] && ok;
    }
    // Detaching A cancels its not-yet-started queue item. Reattach explicitly
    // after the batch, and test an active duplicate in a separate batch.
    observeWin32("SetEvent/loader-release", SetEvent(loader.gate) != FALSE);
    if (!wait([&] { return !controller.snapshot().cleanupPending; })) return 75;
    if (!controller.startBatch({{instances[1]}, {instances[1]}}) ||
        !wait([&] { return !controller.batchStatus().running; })) return 76;
    ok = controller.batchStatus().items.size() == 1 && controller.batchStatus().items[0].state == State::Skipped && ok;
    const auto oldA = ids[0];
    if (!controller.start(targets[0].pid, {}, wa::PipelineFlow::Render, instances[0]) ||
        !wait([&] { return store.activeAttach(targets[0].pid) != 0; })) return 82;
    const auto newA = store.activeAttach(targets[0].pid);
    ok = newA != oldA && store.segment(newA)->methods == updatedMethods && selection.selected() == oldA && ok;
    std::printf("reattach pid=%lu old=%llu new=%llu inherited_methods=%u selected_history=%llu\n",
        targets[0].pid, oldA, newA, store.segment(newA)->methods, selection.selected());
    observeWin32("SetEvent/startup-release", SetEvent(startupGate.gate) != FALSE);
    const int64_t startupReleased = integrationCounter();
    const int64_t intersectionStart = (std::max)({aAttached, startupHeld, batchStarted});
    const int64_t intersectionEnd = (std::min)({aDetached, startupReleased, batchEnded});
    std::printf("interval a_start=%lld a_end=%lld b_start=%lld b_end=%lld batch_start=%lld batch_end=%lld "
        "intersection_start=%lld intersection_end=%lld positive=%d\n", aAttached, aDetached, startupHeld,
        startupReleased, batchStarted, batchEnded, intersectionStart, intersectionEnd, intersectionStart < intersectionEnd);
    ok = intersectionStart < intersectionEnd && ok;
    if (!wait([&] { return !controller.startupStatus().running && b.shared->ready != 0; })) return 77;
    startup = controller.startupStatus();
    ok = startup.outcome.committed && startup.outcome.result && marker.crtEntered == 1 && marker.mainEntered == 1 &&
        marker.tlsCounter < marker.crtCounter && marker.crtCounter <= marker.mainCounter && ok;
    std::printf("startup_marker committed_qpc=%lld tls=%u crt=%u main=%u tls_qpc=%lld crt_qpc=%lld main_qpc=%lld committed=%d\n",
        integrationCounter(), marker.tlsEntered, marker.crtEntered, marker.mainEntered, marker.tlsCounter,
        marker.crtCounter, marker.mainCounter, startup.outcome.committed);
    for (size_t index = 0; index < targets.size(); ++index) {
        ids[index] = store.latestAttach(instances[index]);
        if (!ids[index] || !loadedHook(targets[index].pid, status.hookPath)) return 78;
        const auto* segment = store.segment(ids[index]);
        std::printf("identity target=%zu pid=%u created=%llu segment=%llu token=%llu methods=%u generation=%u\n", index,
            instances[index].pid, instances[index].created, segment->id, segment->token, segment->methods, segment->observationGeneration);
    }
    if (!wait([&] { return store.segment(ids[1])->windowRetained == 1024 && store.segment(ids[2])->windowRetained == 1024; })) return 79;
    for (size_t index = 0; index < targets.size(); ++index) {
        const auto& segment = *store.segment(ids[index]);
        uint64_t completed = 0, failed = 0, discontinuities = 0;
        for (const auto& method : segment.transport.statistics.methods) {
            completed += method.completed;
            failed += method.failed;
            discontinuities += method.discontinuities;
        }
        std::printf("window target=%zu pid=%u segment=%llu controls=%zu retained=%zu received=%llu evicted=%llu "
            "methods=%u generation=%u statistics_known=%d completed=%llu failed=%llu discontinuities=%llu\n",
            index, segment.instance.pid, segment.id, segment.count, segment.windowRetained, segment.windowReceived,
            segment.windowEvicted, segment.methods, segment.observationGeneration, segment.transport.statistics.known,
            completed, failed, discontinuities);
    }
    const auto frozenMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto snapshot = store.freezeExport(ids[1], frozenMs);
    const auto before = frozenText(snapshot);
    std::ofstream(output / L"frozen-before.jsonl", std::ios::binary) << before;
    const auto generation = store.segment(ids[1])->observationGeneration;
    const auto otherReceived = store.segment(ids[2])->windowReceived;
    selection.select(store, ids[2]);
    if (!controller.clear(instances[1], store.segment(ids[1])->token) || !wait([&] {
        return cleared == 1 && store.segment(ids[1])->observationGeneration != generation &&
            store.segment(ids[1])->windowReceived > 10 && store.segment(ids[2])->windowReceived > otherReceived;
    })) return 80;
    const auto after = frozenText(snapshot);
    std::ofstream(output / L"frozen-after.jsonl", std::ios::binary) << after;
    ok = before == after && ok;
    wa::call_log_filter::FilterOpts query;
    query.attach = ids[1];
    const auto view = store.view(query);
    for (size_t row = 0; row < view.size(); ++row)
        ok = view.at(row)->call.observationGeneration == store.segment(ids[1])->observationGeneration && ok;
    std::printf("frozen_export stable=%d rows=%zu clear_generation_before=%u after=%u cleared=%u "
        "other_received_before=%llu after=%llu\n", before == after, snapshot.records.size(), generation,
        store.segment(ids[1])->observationGeneration, cleared, otherReceived, store.segment(ids[2])->windowReceived);
    std::array<uint64_t, 4> original{}, frames{};
    for (size_t index = 0; index < targets.size(); ++index) {
        original[index] = targets[index].shared->stats[0].originalCalls;
        frames[index] = targets[index].shared->stats[0].capturedFrames;
    }
    controller.shutdown();
    Sleep(300);
    for (size_t index = 0; index < targets.size(); ++index) {
        const auto& stats = targets[index].shared->stats[0];
        const bool advanced = WaitForSingleObject(targets[index].process, 0) == WAIT_TIMEOUT &&
            stats.originalCalls > original[index] && stats.capturedFrames > frames[index];
        std::printf("after_shutdown target=%zu pid=%lu calls_before=%llu after=%llu frames_before=%llu after=%llu advanced=%d\n",
            index, targets[index].pid, original[index], stats.originalCalls, frames[index], stats.capturedFrames, advanced);
        ok = advanced && ok;
        observeWin32("InterlockedExchange/stop", InterlockedExchange(&targets[index].shared->stop, 1) >= 0);
    }
    for (auto& target : targets) {
        const DWORD waited = integrationWait("WaitForSingleObject/integration-audio", target.process, 10000);
        DWORD code = STILL_ACTIVE;
        const bool queried = observeWin32("GetExitCodeProcess/integration-audio", GetExitCodeProcess(target.process, &code) != FALSE);
        const auto& stats = target.shared->stats[0];
        std::printf("audio_exit pid=%lu waited=%lu code=%lu callbacks=%llu failures=%llu discontinuities=%llu frames=%llu\n",
            target.pid, waited, code, stats.callbacks, stats.failures, stats.discontinuities, stats.capturedFrames);
        ok = waited == WAIT_OBJECT_0 && queried && code == 0 && stats.failures == 0 && ok;
    }
    ok = loader.finish() && exiting.finish() && ok;
    std::printf("integration result=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 81;
}
}

int wmain(int argc, wchar_t** argv) {
    integrationStartupMarker(2);
    if (argc >= 3 && !std::wcscmp(argv[1], L"--victim")) return hookAudioProbeMain(3, argv);
    ProbeLog logging;
    if (argc == 3 && !std::wcscmp(argv[1], L"--output")) return integration(argv[2]);
    std::fprintf(stderr, "Usage: PipelineIntegrationProbe --output DIRECTORY\n");
    return 1;
}
