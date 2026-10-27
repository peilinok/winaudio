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
        scenario != "monitor-dump") return false;
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
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (elapsed >= warmup + duration) return false;
    if (monitorDump()) {
        ui.frameProbe_.setStage("monitor-dump");
        return true;
    }
    if (inspector()) {
        warming = elapsed < warmup;
        ui.frameProbe_.setStage(warming ? "warmup" : scenario.c_str());
        due = !warming && elapsed - warmup >= static_cast<double>(nextOperation);
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
bool PerformanceFixture::liveInspector() { return scenario == "live"; }

bool PerformanceFixture::startInspector(AppUi& ui) {
    if (liveInspector()) {
        const char* pids = std::getenv("WINAUDIO_PERF_LIVE_PIDS");
        if (!pids) return false;
        const char* pump = std::getenv("WINAUDIO_PERF_PUMP");
        ui.pipelinePump_ = pump && std::strcmp(pump, "1") == 0;
        ui.refreshPipelineSessions();
        std::istringstream input(pids);
        uint32_t pid = 0;
        while (input >> pid) {
            for (size_t i = 0; i < ui.pipelineSessions_.size(); ++i) {
                if (ui.pipelineSessions_[i].processId == pid) {
                    ui.pipelineSelected_ = static_cast<int>(i);
                    ui.pipelineSelectedPid_ = pid;
                    break;
                }
            }
            std::snprintf(ui.pipelineAttachPid_, sizeof(ui.pipelineAttachPid_), "%u", pid);
            ui.runPipelineAttach();
            if (!ui.pipelineAttaches_.attached(pid)) return false;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
            WA_LOG(wa::log::Level::Debug, "PerformanceFixture", "CreateToolhelp32Snapshot",
                   std::to_string(pid), snapshot == INVALID_HANDLE_VALUE ? "failed" : "ok");
            if (snapshot == INVALID_HANDLE_VALUE) return false;
            MODULEENTRY32W module{};
            module.dwSize = sizeof(module);
            bool found = false;
            if (Module32FirstW(snapshot, &module)) do {
                if (_wcsicmp(module.szModule, L"WinAudioHook-5.dll") == 0) {
                    char path[1024]{};
                    WideCharToMultiByte(CP_UTF8, 0, module.szExePath, -1, path, sizeof(path), nullptr, nullptr);
                    WA_LOG(wa::log::Level::Info, "PerformanceFixture", "loaded-hook", std::to_string(pid), path);
                    found = true;
                }
            } while (Module32NextW(snapshot, &module));
            CloseHandle(snapshot);
            if (!found) return false;
        }
        return ui.pipelineAttaches_.attachedPids().size() == 2;
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

void PerformanceFixture::prepareInspector(AppUi& ui) {
    if (!inspector()) return;
    if (liveInspector()) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        // Ten full minutes with the default off state, then a separate short on check.
        if (!warming && duration >= 620 && elapsed >= warmup + 600) {
            ui.frameProbe_.setStage("live-pump");
            if (!ui.pipelinePump_) {
                ui.pipelinePump_ = true;
                ui.pipelineAttaches_.setPumpEnabled(true);
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
    ui.frameProbe_.operationReady(operationId);
    operationId = 0;
}
