#include "HookModuleIdentity.h"
#include "PerformanceFixture.h"
#include "AppUi.h"
#include "Log.h"
#include "GuiGpu.h"
#include "PerformanceCalls.h"
#include "WavFile.h"
#include "ComUtil.h"
#include <d3d11.h>
#include <tlhelp32.h>
#include "imgui.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <sstream>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace {
std::chrono::steady_clock::time_point started;
int tracks = 2;
double warmup = 30, duration = 120;
int phase = 0;
bool warming = true;
bool manualCharts = false;
std::array<std::array<float, 2>, 4> positions{};
const char* stages[] = {"track1-wave", "track1-spec", "track2-wave", "track2-spec"};
std::string scenario = "charts";
size_t recordCount = 10000, appendNext = 0, nextOperation = 0;
unsigned pidCount = 2;
std::vector<wa::HookedCall> pendingCalls;
bool editFilter = false, scrollPending = false, paramsPending = false;
float tableTarget = 0, paramsTarget = 0;
uint64_t operationId = 0;
uint64_t submittedAfter = 0;
LARGE_INTEGER origin{}, frequency{};
bool due = false;
int warmupAction = -1;
std::vector<uint32_t> livePids;
size_t liveNext = 0;
bool livePending = false, liveReady = false, fixtureFailed = false, fixtureComplete = false;
std::array<uint64_t, 2> liveSegments{};
std::array<std::string, 2> liveHookPaths{};
std::chrono::steady_clock::time_point preparing;
FILE* fixtureTrace = nullptr;
uint64_t formalStart = 0, formalEnd = 0, operationSegment = 0;
uint32_t operationGeneration = 0, operationMethods = 0;
size_t pipelineAction = 0;
bool restorePending = false;
bool visibilityKnown = false, wasOccluded = false, wasMinimized = false;
std::vector<wa::HookedCall> fixtureControls;
bool evidenceInspector() { return scenario == "pipeline" || scenario == "pressure" || scenario == "steady"; }
const char* pipelineKinds[] = {"switch", "configure", "filter", "scroll", "details", "clear", "export-start"};
uint64_t fixtureTick() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return static_cast<uint64_t>(value.QuadPart); }
void trace(const char* event, uint64_t id = 0, uint64_t segment = 0, uint64_t value = 0) {
    if (fixtureTrace) std::fprintf(fixtureTrace, "%s,%llu,%llu,%llu,%llu\n", event, fixtureTick(), id, segment, value);
}

// Delays only the export worker, exercising the real GUI lifecycle without disk faults.
class ControlledExportWriter : public wa::CallLogWriter {
public:
    explicit ControlledExportWriter(bool fail) : fail_(fail) {}
    wa::Result open(const std::wstring&) override {
        first_ = true;
        WA_LOG(wa::log::Level::Info, "PerformanceFixture", "export-test-open", "controlled writer", "no real file");
        return wa::Result::Ok();
    }
    wa::Result write(std::string_view) override {
        if (first_) {
            first_ = false;
            WA_LOG(wa::log::Level::Info, "PerformanceFixture", "export-test-write",
                   fail_ ? "fail after 60 seconds" : "return after 60 seconds", "worker only; no real file");
            std::this_thread::sleep_for(std::chrono::seconds(60));
            if (fail_) return wa::Result::Fail(E_FAIL, "controlled export write failure (test only)");
        }
        return wa::Result::Ok();
    }
    wa::Result flush() override { return wa::Result::Ok(); }
    wa::Result close() override { return wa::Result::Ok(); }
    wa::Result publish() override {
        WA_LOG(wa::log::Level::Info, "PerformanceFixture", "export-test-publish", "simulated", "no real file");
        return wa::Result::Ok();
    }
    wa::Result cleanup() override { return wa::Result::Ok(); }
private:
    bool fail_, first_ = true;
};

// Wrap the real WAV writer; only this opt-in executable can delay file close.
// A named event lets the smoke driver release the writer after inspecting the
// production Monitor controls. The pump and GUI never wait on this event.
class ControlledMonitorOutput : public wa::WavSinkOutput {
public:
    explicit ControlledMonitorOutput(const std::wstring& eventName) : eventName_(eventName) {}
    wa::Result open(const std::wstring& path, const wa::AudioFormat& format) override {
        const wa::Result result = writer_.open(path, format);
        opened_ = result.ok;
        return result;
    }
    wa::Result setBuffer(size_t bytes) override { return writer_.setStdioBuffer(bytes); }
    wa::Result write(const void* data, size_t bytes) override { return writer_.write(data, bytes); }
    wa::Result close() override {
        if (!opened_) return writer_.close();
        opened_ = false;
        wa::Result gate = wa::Result::Ok();
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, eventName_.c_str());
        const DWORD createError = event ? ERROR_SUCCESS : GetLastError();
        WA_LOG(wa::log::Level::Debug, "PerformanceFixture", "CreateEventW",
               "Monitor WAV close gate", wa::log::hrName(HRESULT_FROM_WIN32(createError)));
        if (!event) {
            gate = wa::HrToResult(HRESULT_FROM_WIN32(createError), "Monitor close gate CreateEventW");
        } else {
            WA_LOG(wa::log::Level::Info, "PerformanceFixture", "monitor-close-wait",
                   "real WAV; bounded event wait", "writer only");
            const DWORD waited = WaitForSingleObject(event, 180000);
            const DWORD waitError = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            WA_LOG(wa::log::Level::Info, "PerformanceFixture", "WaitForSingleObject",
                   "Monitor WAV close gate", std::to_string(waited));
            if (waited == WAIT_FAILED)
                gate = wa::HrToResult(HRESULT_FROM_WIN32(waitError), "Monitor close gate wait");
            else if (waited != WAIT_OBJECT_0)
                gate = wa::Result::Fail(-1, "Monitor close gate timed out (test only)");
            const BOOL released = CloseHandle(event);
            const DWORD closeError = released ? ERROR_SUCCESS : GetLastError();
            WA_LOG(released ? wa::log::Level::Debug : wa::log::Level::Warn,
                   "PerformanceFixture", "CloseHandle", "Monitor WAV close gate",
                   wa::log::hrName(HRESULT_FROM_WIN32(closeError)));
        }
        const wa::Result closed = writer_.close();
        return gate ? closed : gate;
    }
private:
    wa::WavWriter writer_;
    std::wstring eventName_;
    bool opened_ = false;
};
}

std::shared_ptr<wa::CallLogWriter> PerformanceFixture::exportWriter() {
    const char* mode = std::getenv("WINAUDIO_PERF_EXPORT_TEST");
    if (mode && (std::strcmp(mode, "wait") == 0 || std::strcmp(mode, "fail") == 0))
        return std::make_shared<ControlledExportWriter>(std::strcmp(mode, "fail") == 0);
    return {};
}

wa::WavSinkOutputFactory PerformanceFixture::monitorRenderOutput() {
    const wchar_t* eventName = _wgetenv(L"WINAUDIO_PERF_MONITOR_CLOSE_EVENT");
    if (!eventName || !*eventName) return {};
    return [name = std::wstring(eventName)] {
        return std::make_unique<ControlledMonitorOutput>(name);
    };
}

bool PerformanceFixture::monitorDump() { return scenario == "monitor-dump"; }

bool PerformanceFixture::start(AppUi& ui) {
    if (const char* value = std::getenv("WINAUDIO_PERF_MANUAL")) manualCharts = std::strcmp(value, "1") == 0;
    if (const char* value = std::getenv("WINAUDIO_PERF_TRACKS")) tracks = std::atoi(value);
    if (const char* value = std::getenv("WINAUDIO_PERF_WARMUP")) warmup = std::atof(value);
    if (const char* value = std::getenv("WINAUDIO_PERF_DURATION")) duration = std::atof(value);
    if ((tracks != 1 && tracks != 2 && tracks != 4) || warmup < 0 || duration <= 0) return false;
    ui.logCollapsed_ = false;
    if (const char* value = std::getenv("WINAUDIO_PERF_MODE")) scenario = value;
    if (scenario != "charts" && scenario != "browse" && scenario != "idle" &&
        scenario != "append" && scenario != "memory" && scenario != "save" && scenario != "live" &&
        scenario != "pipeline" && scenario != "pressure" && scenario != "steady" && scenario != "monitor-dump") return false;
    if (evidenceInspector()) {
        if (fopen_s(&fixtureTrace, "fixture-events.csv", "wb") != 0 || !fixtureTrace) return false;
        std::fprintf(fixtureTrace, "event,qpc,id,segment,value\n");
        trace("prepare-start");
    }
    if (monitorDump()) {
        started = std::chrono::steady_clock::now();
        return true;
    }
    if (inspector()) {
        if (!startInspector(ui)) return false;
    } else {
        wa::RenderTrackCreate recipe;
        const wchar_t* endpoint = _wgetenv(L"WINAUDIO_PERF_ENDPOINT");
        if (!endpoint || !*endpoint) return false;
        recipe.deviceId = endpoint;
        recipe.source = {48000, 8, 32, true, 32, 0x63f};
        recipe.mode = wa::RenderLayoutMode::SystemDefault;
        for (int i = 0; i < tracks; ++i) {
            wa::TrackId id = 0;
            const auto result = ui.renderTracks_.create(recipe, &id);
            if (!result) {
                WA_LOG(wa::log::Level::Err, "PerformanceFixture", "create", "RenderTrack", result.message);
                return false;
            }
            for (uint16_t channel = 0; channel < 8; ++channel) {
                if (!ui.renderTracks_.setVolume(id, channel, i % 2 == 0 ? 65 : 25) ||
                    !ui.renderTracks_.continueChannel(id, channel)) return false;
            }
        }
        for (const auto& status : ui.renderTracks_.poll()) {
            if (status.state != wa::StreamState::Running || status.chartChannels != 8) return false;
            WA_LOG(wa::log::Level::Info, "PerformanceFixture", "track", std::to_string(status.id),
                   std::to_string(status.actualFormat.sampleRate) + "Hz channels=" +
                   std::to_string(status.chartChannels));
        }
    }
    RECT client{};
    const HWND hwnd = static_cast<HWND>(ui.pipelineHwnd_);
    const BOOL haveClient = GetClientRect(hwnd, &client);
    const UINT dpi = GetDpiForWindow(hwnd);
    WA_LOG(wa::log::Level::Info, "PerformanceFixture", "GetClientRect/GetDpiForWindow",
           std::to_string(haveClient), std::to_string(client.right - client.left) + "x" +
           std::to_string(client.bottom - client.top) + " dpi=" + std::to_string(dpi));
    IDXGIDevice* dxgi = nullptr;
    HRESULT hr = wa::guiDevice()->QueryInterface(IID_PPV_ARGS(&dxgi));
    WA_LOG(wa::log::Level::Debug, "PerformanceFixture", "QueryInterface", "IDXGIDevice", wa::log::hrName(hr));
    if (SUCCEEDED(hr)) {
        IDXGIAdapter* adapter = nullptr;
        hr = dxgi->GetAdapter(&adapter);
        WA_LOG(wa::log::Level::Debug, "PerformanceFixture", "GetAdapter", "DXGI", wa::log::hrName(hr));
        if (SUCCEEDED(hr)) {
            DXGI_ADAPTER_DESC desc{};
            hr = adapter->GetDesc(&desc);
            char name[256]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            WA_LOG(wa::log::Level::Info, "PerformanceFixture", "GetDesc", wa::log::hrName(hr), name);
            adapter->Release();
        }
        dxgi->Release();
    }
    started = std::chrono::steady_clock::now();
    QueryPerformanceCounter(&origin);
    QueryPerformanceFrequency(&frequency);
    return true;
}

bool PerformanceFixture::advance(AppUi& ui) {
    if (const wchar_t* cancel = _wgetenv(L"WINAUDIO_PERF_OWNER_CANCEL"))
        if (std::filesystem::exists(cancel)) { fixtureFailed = true; trace("cancelled"); return false; }
    if (fixtureFailed) return false;
    if (liveInspector() && !liveReady) {
        ui.frameProbe_.setStage("preparing");
        if (std::chrono::steady_clock::now() - preparing > std::chrono::seconds(120)) {
            fixtureFailed = true;
            trace("prepare-timeout");
            return false;
        }
        return true;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (elapsed >= warmup + duration) {
        if (evidenceInspector() && elapsed < warmup + duration + .2) {
            ui.frameProbe_.setStage("finalizing");
            return true;
        }
        fixtureComplete = true;
        if (evidenceInspector()) {
            ui.pollPipelineAttach(true);
            std::ofstream summaryFile("fixture-final-summary.jsonl");
            for (const auto id : liveSegments) {
                const auto* segment = ui.pipelineCallStore_.segment(id);
                if (!segment || segment->windowRetained != 1024) fixtureFailed = true;
                else {
                    trace("final-controls", 0, id, segment->count);
                    trace("final-received", 0, id, segment->windowReceived);
                    for (const auto& count : segment->transport.statistics.methods)
                        if (count.unknown) fixtureFailed = true;
                    const auto snapshot = ui.pipelineCallStore_.freezeExport(id, segment->transport.snapshotMs);
                    for (const auto& line : wa::CallLogStore::exportSummary(snapshot, false)) summaryFile << line << '\n';
                }
            }
            summaryFile.close();
            std::ofstream windowFile("fixture-final-windows.jsonl");
            for (const auto& record : ui.pipelineCallStore_.pumpEntries())
                windowFile << wa::CallLogStore::toJsonLine(*record) << '\n';
            windowFile.close();
            if (!windowFile || !summaryFile || (scenario == "steady" && ui.pipelineCallStore_.size())) fixtureFailed = true;
            if (std::getenv("WINAUDIO_PERF_SOURCE_DIRECTORY")) {
                std::ofstream finalFlag("gui-final.flag");
                finalFlag << "source interval ended; final production controller snapshot saved\n";
                finalFlag.close();
                if (!finalFlag) fixtureFailed = true;
            }
            trace("formal-end", operationId, 0, formalEnd);
            if (operationId) fixtureFailed = true;
            if (fixtureTrace) { std::fclose(fixtureTrace); fixtureTrace = nullptr; }
        }
        return false;
    }
    if (monitorDump()) {
        ui.frameProbe_.setStage("monitor-dump");
        return true;
    }
    if (inspector()) {
        warming = elapsed < warmup;
        ui.frameProbe_.setStage(warming ? "warmup" : scenario.c_str());
        due = !warming && elapsed - warmup >= static_cast<double>(nextOperation) * (pipeline() ? .6 : 1.0);
        return true;
    }
    const int count = tracks == 1 ? 2 : 4;
    phase = elapsed < warmup ? 0 : static_cast<int>((elapsed - warmup) / (duration / count));
    if (phase >= count) phase = count - 1;
    warming = elapsed < warmup;
    ui.frameProbe_.setStage(warming ? "warmup" : stages[phase]);
    return true;
}

void PerformanceFixture::scrollCharts(AppUi& ui) {
    if (manualCharts) return;
    const float target = positions[static_cast<size_t>(phase / 2)][static_cast<size_t>(phase % 2)];
    if (!warming && std::abs(ImGui::GetScrollY() - target) > 1.f)
        ui.frameProbe_.setStage("transition");
    ImGui::SetScrollY(target);
}

void PerformanceFixture::chartPosition(uint64_t source, bool spectrum) {
    if (source == 0 || source > positions.size()) return;
    positions[static_cast<size_t>(source - 1)][spectrum ? 1u : 0u] = ImGui::GetCursorPosY();
}

bool PerformanceFixture::inspector() { return scenario != "charts" && !monitorDump(); }
bool PerformanceFixture::liveInspector() { return scenario == "live" || evidenceInspector(); }
bool PerformanceFixture::pipeline() { return scenario == "pipeline"; }
bool PerformanceFixture::successful() { return !fixtureFailed && (!evidenceInspector() || fixtureComplete || manualCharts); }
void PerformanceFixture::visibility(bool occluded, bool minimized) {
    if (!evidenceInspector()) return;
    if (!visibilityKnown || wasOccluded != occluded || wasMinimized != minimized) {
        trace(occluded ? "occluded" : "visible", minimized ? 1 : 0);
        visibilityKnown = true;
        wasOccluded = occluded;
        wasMinimized = minimized;
    }
    const auto now = fixtureTick();
    if ((occluded || minimized) && liveReady && now >= formalStart && now < formalEnd) fixtureFailed = true;
}

bool PerformanceFixture::startInspector(AppUi& ui) {
    if (liveInspector()) {
        const char* pids = std::getenv("WINAUDIO_PERF_LIVE_PIDS");
        if (!pids) return false;
        const char* pump = std::getenv("WINAUDIO_PERF_PUMP");
        ui.pipelinePump_ = pump && std::strcmp(pump, "1") == 0;
        ui.refreshPipelineSessions();
        std::istringstream input(pids);
        uint32_t pid = 0;
        while (input >> pid) livePids.push_back(pid);
        if (livePids.size() != 2 || livePids[0] == livePids[1]) return false;
        preparing = std::chrono::steady_clock::now();
        ui.pipelinePump_ = evidenceInspector() || ui.pipelinePump_;
        prepareLive(ui);
        return !fixtureFailed;
    }
    if (const char* value = std::getenv("WINAUDIO_PERF_RECORDS")) recordCount = std::strtoul(value, nullptr, 10);
    if (const char* value = std::getenv("WINAUDIO_PERF_PIDS")) pidCount = static_cast<unsigned>(std::atoi(value));
    if ((recordCount != 1000 && recordCount != 10000) || (pidCount != 1 && pidCount != 2)) return false;
    auto calls = wa::performanceCalls(recordCount, pidCount);
    if (calls.size() != recordCount) return false;
    FILE* input = nullptr;
    if (fopen_s(&input, "fixture-input.jsonl", "wb") != 0 || !input) return false;
    bool written = true;
    for (const auto& call : calls) {
        const std::string line = wa::CallLogStore::toJsonLine(call) + '\n';
        if (std::fwrite(line.data(), 1, line.size(), input) != line.size()) written = false;
    }
    if (std::fclose(input) != 0) written = false;
    if (!written) return false;
    const size_t initial = scenario == "append" ? 9000 : calls.size();
    if (initial > calls.size()) return false;
    pendingCalls.assign(std::make_move_iterator(calls.begin() + static_cast<std::ptrdiff_t>(initial)),
                        std::make_move_iterator(calls.end()));
    calls.resize(initial);
    // Fixed session context for the production graph join; no injection or COM probes.
    ui.pipelineSessions_.clear();
    for (unsigned i = 0; i < pidCount; ++i) {
        wa::LiveSessionView session;
        session.processId = 42001 + i;
        session.processName = "Inspector fixture " + std::to_string(i + 1);
        session.sessionInstanceId = "fixture-" + std::to_string(i);
        session.flow = wa::PipelineFlow::Render;
        session.deviceName = "Fixed context (not a live endpoint)";
        session.state = "Active";
        ui.pipelineSessions_.push_back(std::move(session));
    }
    ui.pipelineSelected_ = 0;
    ui.pipelineSelectedPid_ = 42001;
    ui.pipelineSessionsLoaded_ = true;
    ui.pipelineAudioOnly_ = true;
    ui.rebuildPipelineProcessTree();
    ui.pipelineTopRowFrac_ = 0.25f;
    ui.receivePipelineCalls(std::move(calls));
    ui.pipelineCallSelected_ = ui.pipelineCallStore_.entries().front()->id;
    return true;
}

void PerformanceFixture::prepareLive(AppUi& ui) {
    if (liveNext < livePids.size()) {
        const auto pid = livePids[liveNext];
        if (!livePending) {
            for (size_t i = 0; i < ui.pipelineSessions_.size(); ++i) if (ui.pipelineSessions_[i].processId == pid) {
                ui.pipelineSelected_ = static_cast<int>(i);
                ui.pipelineSelectedPid_ = pid;
                break;
            }
            std::snprintf(ui.pipelineAttachPid_, sizeof(ui.pipelineAttachPid_), "%u", pid);
            ui.runPipelineAttach();
            livePending = true;
            return;
        }
        if (ui.pipelineAttachFailByPid_.count(pid)) { fixtureFailed = true; trace("attach-failed", 0, pid); return; }
        if (!ui.pipelineAttaches_.attached(pid)) return;
        const auto id = ui.pipelineCallStore_.activeAttach(pid);
        const auto* segment = ui.pipelineCallStore_.segment(id);
        if (!segment) return;
        if (evidenceInspector() && segment->methods != wa::kAllMonitorMethods) {
            auto& draft = ui.pipelineMethodDrafts_[id];
            if (!draft.pending) {
                draft.methods = wa::kAllMonitorMethods;
                const auto result = ui.pipelineAttaches_.configure(segment->instance, segment->token, draft.methods);
                draft.pending = static_cast<bool>(result);
                if (!result) fixtureFailed = true;
            }
            return;
        }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        WA_LOG(wa::log::Level::Debug, "PerformanceFixture", "CreateToolhelp32Snapshot", std::to_string(pid),
            snapshot == INVALID_HANDLE_VALUE ? wa::log::hrName(HRESULT_FROM_WIN32(GetLastError())) : "ok");
        if (snapshot == INVALID_HANDLE_VALUE) { fixtureFailed = true; return; }
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        unsigned found = 0;
        if (Module32FirstW(snapshot, &module)) do {
            if (wa::isHookModuleName(module.szModule)) {
                char path[1024]{};
                WideCharToMultiByte(CP_UTF8, 0, module.szExePath, -1, path, sizeof(path), nullptr, nullptr);
                WA_LOG(wa::log::Level::Info, "PerformanceFixture", "loaded-hook", std::to_string(pid), path);
                liveHookPaths[liveNext] = path;
                ++found;
            }
        } while (Module32NextW(snapshot, &module));
        const BOOL closed = CloseHandle(snapshot);
        WA_LOG(closed ? wa::log::Level::Debug : wa::log::Level::Warn, "PerformanceFixture", "CloseHandle", "module snapshot",
            closed ? "ok" : wa::log::hrName(HRESULT_FROM_WIN32(GetLastError())));
        if (found != 1) { fixtureFailed = true; return; }
        liveSegments[liveNext] = id;
        trace("attached", pid, id, segment->instance.created);
        ++liveNext;
        livePending = false;
        return;
    }
    if (pipeline() && fixtureControls.empty()) {
        fixtureControls = wa::performanceCalls(10000, 2);
        std::ofstream inputFile("fixture-input.jsonl");
        for (size_t i = 0; i < fixtureControls.size(); ++i) {
            auto call = fixtureControls[i];
            const auto* segment = ui.pipelineCallStore_.segment(liveSegments[i % 2]);
            call.pid = segment->instance.pid;
            call.processCreated = segment->instance.created;
            call.attachToken = segment->token;
            call.observationGeneration = segment->observationGeneration;
            inputFile << wa::CallLogStore::toJsonLine(call) << '\n';
        }
        inputFile.close();
        if (!inputFile) { fixtureFailed = true; return; }
        for (const auto id : liveSegments) restoreControls(ui, id);
        ui.pipelineTopRowFrac_ = .25f;
    }
    uint64_t startTick = fixtureTick();
    QueryPerformanceFrequency(&frequency);
    if (const wchar_t* source = _wgetenv(L"WINAUDIO_PERF_SOURCE_DIRECTORY")) {
        const std::filesystem::path directory(source);
        if (!std::filesystem::exists(directory / "gui-ready.flag")) {
            std::ofstream ready(directory / "gui-ready.flag");
            ready << "production controller attached and configured both explicit targets\n";
            ready.close();
            trace("source-ready");
        }
        std::ifstream originFile(directory / "source-start.txt");
        if (!(originFile >> startTick)) return;
    }
    const auto now = fixtureTick();
    started = std::chrono::steady_clock::now() + std::chrono::nanoseconds(
        (static_cast<int64_t>(startTick) - static_cast<int64_t>(now)) * 1000000000ll / frequency.QuadPart);
    origin.QuadPart = static_cast<LONGLONG>(startTick);
    formalStart = startTick + static_cast<uint64_t>(warmup * frequency.QuadPart);
    formalEnd = formalStart + static_cast<uint64_t>(duration * frequency.QuadPart);
    liveReady = true;
    trace("warmup-start", 0, 0, startTick);
    trace("formal-start", 0, 0, formalStart);
    trace("formal-planned-end", 0, 0, formalEnd);
    if (fixtureTrace) std::fflush(fixtureTrace);
    // Unlike the buffered trace, this closed file can be read while the GUI runs.
    std::ofstream ready("gui-ready.json");
    ready << "{\"gui_pid\":" << GetCurrentProcessId() << ",\"qpc_frequency\":" << frequency.QuadPart
        << ",\"warmup_start\":" << startTick << ",\"formal_start\":" << formalStart
        << ",\"formal_end\":" << formalEnd << ",\"targets\":[";
    for (size_t i = 0; i < liveSegments.size(); ++i) {
        const auto* segment = ui.pipelineCallStore_.segment(liveSegments[i]);
        ready << (i ? "," : "") << "{\"pid\":" << segment->instance.pid << ",\"created_filetime\":"
            << segment->instance.created << ",\"segment\":" << segment->id << ",\"token\":" << segment->token
            << ",\"generation\":" << segment->observationGeneration << ",\"hook_path\":"
            << std::quoted(liveHookPaths[i]) << "}";
    }
    ready << "]}\n";
    ready.close();
    if (!ready) fixtureFailed = true;
}

void PerformanceFixture::restoreControls(AppUi& ui, uint64_t attach) {
    const auto* segment = ui.pipelineCallStore_.segment(attach);
    if (!segment) { fixtureFailed = true; return; }
    const auto index = static_cast<size_t>(std::find(liveSegments.begin(), liveSegments.end(), attach) - liveSegments.begin());
    std::vector<wa::HookedCall> calls;
    calls.reserve(5000);
    for (size_t i = index; i < fixtureControls.size(); i += 2) {
        auto call = fixtureControls[i];
        call.pid = segment->instance.pid;
        call.processCreated = segment->instance.created;
        call.attachToken = segment->token;
        call.observationGeneration = segment->observationGeneration;
        calls.push_back(std::move(call));
    }
    ui.pipelineCallStore_.append(attach, std::move(calls));
    ui.applyPipelineJoin();
    trace("history-restored", operationId, attach, ui.pipelineCallStore_.segment(attach)->count);
}

void PerformanceFixture::preparePipeline(AppUi& ui) {
    if (operationId && pipelineAction == 5) {
        const auto* segment = ui.pipelineCallStore_.segment(operationSegment);
        if (segment && segment->observationGeneration != operationGeneration &&
            !ui.pipelineClearStates_[operationSegment].pending && restorePending) {
            trace("clear-effective", operationId, operationSegment, segment->observationGeneration);
            restoreControls(ui, operationSegment);
            restorePending = false;
        }
    }
    if (warming || !due || operationId || manualCharts || duration -
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() + warmup < 2.4) return;
    const uint64_t issue = static_cast<uint64_t>(origin.QuadPart) +
        static_cast<uint64_t>((warmup + static_cast<double>(nextOperation) * .6) * frequency.QuadPart);
    operationId = ++nextOperation;
    pipelineAction = (nextOperation - 1) % std::size(pipelineKinds);
    operationSegment = ui.pipelineSegmentSelection_.selected();
    const auto* segment = ui.pipelineCallStore_.segment(operationSegment);
    if (!segment || !ui.frameProbe_.startOperation(operationId, pipelineKinds[pipelineAction], issue)) { fixtureFailed = true; return; }
    submittedAfter = ui.frameProbe_.frameNumber();
    trace("command-issued", operationId, operationSegment, pipelineAction);
    if (pipelineAction == 0) {
        const auto id = operationSegment == liveSegments[0] ? liveSegments[1] : liveSegments[0];
        ui.pipelineSegmentSelection_.select(ui.pipelineCallStore_, id);
        ui.pipelineCallSelected_ = {};
    } else if (pipelineAction == 1) {
        auto& draft = ui.pipelineMethodDrafts_[operationSegment];
        operationMethods = segment->methods ^ wa::methodBit(wa::MonitorMethod::SetMute);
        draft.methods = operationMethods;
        const auto result = ui.pipelineAttaches_.configure(segment->instance, segment->token, operationMethods);
        draft.pending = static_cast<bool>(result);
        if (!result) fixtureFailed = true;
    } else if (pipelineAction == 2) {
        const bool set = ui.pipelineCallFilter_[0] == '\0';
        std::snprintf(ui.pipelineCallFilter_, sizeof(ui.pipelineCallFilter_), "%s", set ? "iacl init" : "");
        editFilter = true;
    } else if (pipelineAction == 3) {
        tableTarget = ((nextOperation / 7) % 2) ? 1e9f : 0.f;
        scrollPending = true;
    } else if (pipelineAction == 4) {
        for (const auto& record : ui.pipelineCallStore_.entries()) if (record->id.attach == operationSegment) {
            ui.pipelineCallSelected_ = record->id;
            ui.pipelineCallScrollToSelected_ = true;
            submittedAfter = ui.frameProbe_.frameNumber() + 1;
            break;
        }
    } else if (pipelineAction == 5) {
        operationGeneration = segment->observationGeneration;
        const auto result = ui.pipelineAttaches_.clear(segment->instance, segment->token);
        ui.pipelineClearStates_[operationSegment].pending = static_cast<bool>(result);
        restorePending = true;
        if (!result) fixtureFailed = true;
    } else if (!ui.savePipelineCallLog(L"fixture-export-" + std::to_wstring(operationId) + L".jsonl")) fixtureFailed = true;
    trace("command-enqueued", operationId, operationSegment, pipelineAction);
}

void PerformanceFixture::prepareInspector(AppUi& ui) {
    if (!inspector()) return;
    if (liveInspector() && !liveReady) { prepareLive(ui); return; }
    if (pipeline()) preparePipeline(ui);
    if (evidenceInspector()) {
        for (const auto id : liveSegments) if (const auto* segment = ui.pipelineCallStore_.segment(id)) {
            ui.frameProbe_.addWork("fixture-controls", segment->count, id);
            ui.frameProbe_.addWork("fixture-window", segment->windowRetained, id);
            ui.frameProbe_.addWork("fixture-window-received", segment->windowReceived, id);
            ui.frameProbe_.addWork("fixture-generation", segment->observationGeneration, id);
            ui.frameProbe_.addWork("fixture-worker-snapshot", segment->transport.snapshotSequence, id);
            uint64_t completed = 0;
            for (const auto& count : segment->transport.statistics.methods) completed += count.completed;
            ui.frameProbe_.addWork("fixture-source-completed", completed, id);
        }
        const auto snapshot = ui.pipelineAttaches_.snapshot();
        ui.frameProbe_.addWork("controller-queued-batches", snapshot.queuedBatches);
        ui.frameProbe_.addWork("controller-queued-controls", snapshot.queuedControls);
        ui.frameProbe_.addWork("controller-queued-pump", snapshot.queuedPump);
        return;
    }
    if (liveInspector()) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        // Ten full minutes with the default off state, then a separate short on check.
        if (!warming && duration >= 620 && elapsed >= warmup + 600) {
            ui.frameProbe_.setStage("live-pump");
            if (!ui.pipelinePump_) {
                ui.pipelinePump_ = true;
                for (const auto& segment : ui.pipelineCallStore_.segments(true))
                    ui.pipelineAttaches_.configure(segment.instance, segment.token, wa::kAllMonitorMethods);
            }
        }
        return;
    }
    if (warming && scenario == "memory") {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const int action = static_cast<int>(seconds / 3);
        if (action != warmupAction) {
            warmupAction = action;
            const int step = action % 6;
            if (step == 0 || step == 1) {
                const auto& records = ui.pipelineCallStore_.entries();
                ui.pipelineCallSelected_ = (step == 0 ? records.front() : records.back())->id;
                ui.pipelineCallScrollToSelected_ = true;
            } else if (step == 2 || step == 3) {
                std::snprintf(ui.pipelineCallFilter_, sizeof(ui.pipelineCallFilter_), "%s", step == 2 ? "iacl init" : "");
                editFilter = true;
            } else if (step == 4) {
                tableTarget = 1e9f;
                scrollPending = true;
            } else {
                paramsTarget = 1e9f;
                paramsPending = true;
            }
        }
    }
    if (!warming && scenario == "append" && appendNext < pendingCalls.size()) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() - warmup;
        const size_t wanted = (std::min)(pendingCalls.size(), static_cast<size_t>(elapsed * 10));
        std::vector<wa::HookedCall> batch;
        while (appendNext < wanted) batch.push_back(std::move(pendingCalls[appendNext++]));
        ui.receivePipelineCalls(std::move(batch));
    }
    if (!due || operationId != 0 || scenario == "idle") return;
    const uint64_t issue = static_cast<uint64_t>(origin.QuadPart) +
        static_cast<uint64_t>((warmup + static_cast<double>(nextOperation)) * frequency.QuadPart);
    operationId = ++nextOperation;
    if (scenario == "save" &&
        (nextOperation == 20 || nextOperation == 21 || nextOperation == 40 || nextOperation == 41 || nextOperation == 100)) {
        const bool saving = nextOperation == 100;
        const bool recording = nextOperation == 20 || nextOperation == 40;
        ui.frameProbe_.startOperation(operationId, saving ? "save" : (recording ? "record-on" : "record-off"), issue);
        if (saving) ui.savePipelineCallLog(L"fixture-export.jsonl");
        else ui.setPipelineRecording(recording);
        return;
    }
    const char* kinds[] = {"select-old", "filter", "clear-filter", "scroll-calls", "select-new", "scroll-parameters"};
    const size_t action = (nextOperation - 1) % 6;
    ui.frameProbe_.startOperation(operationId, kinds[action], issue);
    if (action == 0) {
        ui.pipelineCallSelected_ = ui.pipelineCallStore_.entries().front()->id;
        ui.pipelineCallScrollToSelected_ = true;
        submittedAfter = ui.frameProbe_.frameNumber() + 1;
    } else if (action == 1 || action == 2) {
        std::snprintf(ui.pipelineCallFilter_, sizeof(ui.pipelineCallFilter_), "%s", action == 1 ? "iacl init" : "");
        editFilter = true;
    } else if (action == 3) {
        tableTarget = ((nextOperation / 6) % 2) ? 1e9f : 0.f;
        scrollPending = true;
    } else if (action == 4) {
        ui.pipelineCallSelected_ = ui.pipelineCallStore_.entries().back()->id;
        ui.pipelineCallScrollToSelected_ = true;
        submittedAfter = ui.frameProbe_.frameNumber() + 1;
    } else {
        paramsTarget = ((nextOperation / 6) % 2) ? 1e9f : 0.f;
        paramsPending = true;
    }
}

bool PerformanceFixture::filterEdited() {
    const bool edited = editFilter;
    editFilter = false;
    return edited;
}

void PerformanceFixture::scrollCalls() {
    if (!scrollPending) return;
    // Freeze the requested position on the first draw. A growing history must
    // not turn one scroll operation into an endless chase of the new bottom.
    tableTarget = (std::min)(tableTarget, ImGui::GetScrollMaxY());
    const float target = tableTarget;
    if (std::abs(ImGui::GetScrollY() - target) <= 1.f) scrollPending = false;
    else ImGui::SetScrollY(target);
}

void PerformanceFixture::scrollParameters() {
    if (!paramsPending) return;
    paramsTarget = (std::min)(paramsTarget, ImGui::GetScrollMaxY());
    const float target = paramsTarget;
    if (std::abs(ImGui::GetScrollY() - target) <= 1.f) paramsPending = false;
    else ImGui::SetScrollY(target);
}

void PerformanceFixture::inspectorSubmitted(AppUi& ui) {
    if (!operationId || scrollPending || paramsPending || ui.frameProbe_.frameNumber() < submittedAfter) return;
    // Selection scrolling uses ImGui's deferred target, so finish on the next draw.
    if (ui.pipelineCallScrollToSelected_) return;
    if (pipeline()) {
        const auto* segment = ui.pipelineCallStore_.segment(operationSegment);
        if (pipelineAction == 1 && (!segment || segment->methods != operationMethods ||
            ui.pipelineMethodDrafts_[operationSegment].pending)) return;
        if (pipelineAction == 5 && (!segment || segment->observationGeneration == operationGeneration || restorePending ||
            ui.pipelineClearStates_[operationSegment].pending)) return;
        trace("effect-visible", operationId, operationSegment, pipelineAction);
    }
    ui.frameProbe_.operationReady(operationId);
    operationId = 0;
}
