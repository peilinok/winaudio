#include "AppUi.h"
#ifdef WA_PERFORMANCE_FIXTURE
#include "PerformanceFixture.h"
#endif
#include <functional>
#include "EndpointEnhancements.h"
#include "EndpointEnhancementsAction.h"
#include "DeviceEnumerator.h"
#include "Elevate.h"
#include "HeatmapRgba.h"
#include "WaveEnvelope.h"
#include "ApplicationLoopbackUiModel.h"
#include "PipelineAttachUiModel.h"
#include "CallLogFilter.h"
#include "PipelineSplit.h"
#include "AppUiText.h"
#include "CaptureChannelView.h"
#include "ChartDataPipeline.h"
#include "ChartsFreezePolicy.h"
#include "ChartsTimeZoomPolicy.h"
#include "ComUtil.h"
#include "DumpUi.h"
#include "OsSoundUi.h"
#include "LogRegionPrefs.h"
#include "LogView.h"
#include "MonitorScopeReader.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "AudioFormatStr.h"
#include "RenderTrackScopeReader.h"
#include "FormatSpec.h"
#include "Log.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <string_view>

static std::string monitorInstanceKey(wa::ProcessInstance instance) {
    return std::to_string(instance.pid) + ":" + std::to_string(instance.created);
}

static void drawMonitorMethods(uint32_t& methods, bool readOnly) {
    ImGui::BeginDisabled(readOnly);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineMethodsDefault)))
        methods = wa::kDefaultMonitorMethods;
    ImGui::EndDisabled();
    const char* previous = "";
    bool open = false;
    for (unsigned i = 0; i < static_cast<unsigned>(wa::MonitorMethod::Count); ++i) {
        const auto& info = wa::kMonitorMethods[i];
        if (std::strcmp(previous, info.iface)) {
            if (open) ImGui::TreePop();
            open = ImGui::TreeNodeEx(info.iface, ImGuiTreeNodeFlags_DefaultOpen);
            previous = info.iface;
        }
        if (!open) continue;
        bool enabled = (methods & (1u << i)) != 0;
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginDisabled(readOnly);
        if (ImGui::Checkbox(info.method, &enabled)) {
            if (enabled) methods |= 1u << i; else methods &= ~(1u << i);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (open) ImGui::TreePop();
}

static std::string wtou(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring utow(const char* u) {
    if (!u || !*u) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u, -1, w.data(), n);
    return w;
}

static wa::MonitorStatus statusFromTrack(const wa::CaptureTrackStatus& t) {
    wa::MonitorStatus s{};
    s.overall = t.state;
    s.capState = t.state;
    s.sampleRate = t.actualFormat.sampleRate;
    s.captureChannels = t.actualFormat.channels;
    s.capXruns = t.overruns;
    s.capWrittenFrames = t.writtenFrames;
    s.capLevel = (t.levelL > t.levelR) ? t.levelL : t.levelR;
    s.silentRenderState = t.silentRenderState;
    return s;
}

namespace {
// Shared analysis / spectrogram constants (used by drawChartsColumn + drawChartPanel).
constexpr size_t kFftWin   = 2048;  // FFT window length (samples)
constexpr size_t kFftHop   = 1024;  // hop between analysis frames = one spectrogram column (50% overlap)
constexpr size_t kCatchup  = 8;     // max analysis frames to fast-forward per poll
constexpr int    kSpecRows = 128;   // spectrogram log-frequency rows
constexpr int    kSpecCols = 480;   // time columns -> kSpecCols*kFftHop = 491520 samples ~= 10.2 s @ 48 kHz
constexpr float  kWaveDbFloor = -60.0f;  // waveform dB display floor (center line reads -inf)
// Zoomed-out envelope style: translucent body + opaque outline (keeps dB min/max LOD readable).
constexpr ImVec4 kWaveColor          = ImVec4(0.40f, 0.89f, 0.59f, 1.00f);  // Audition green
constexpr float  kWaveEnvFillAlpha   = 0.55f;  // medium transparency (0.45–0.60 band)
constexpr float  kWaveEnvLineWeight  = 1.0f;   // 1 px min/max outline
constexpr uint32_t kMaxCaptureChannelsShown = wa::capture_channel_view::kMaxCaptureChannelsShown;
// Waveform shares the spectrogram's time window: kSpecCols*kFftHop samples (see drawChartsColumn).
// Chart panel heights (px).
constexpr float  kComboH    = 400.0f;  // waveform+spectrogram combo cell content (split by comboRatio_)
constexpr float  kSplitH    = 6.0f;    // draggable splitter between waveform and spectrogram
constexpr float  kMultiWaveH = 120.0f;
constexpr float  kMultiSpecH = 180.0f;
// Plot slots for plotHovPrev_ (per-plot last-frame plot-area hover; see AppUi.h).
enum : int {
    kSlotCapWaveBase = 0,
    kSlotRenWave = kSlotCapWaveBase + (int)kMaxCaptureChannelsShown,
    kSlotCapSpectroBase,
    kSlotRenSpectro = kSlotCapSpectroBase + (int)kMaxCaptureChannelsShown
};

// Map a linear sample onto the symmetric dB display scale (Audition-style logarithmic waveform):
// |x| in dBFS, warped so 0 dBFS -> +/-1 and kWaveDbFloor or quieter (incl. silence) -> 0 (the
// center line reads -inf). Monotonic, so min/max envelopes commute with the warp.
inline float dbWarp(float x) {
    // Shared with WaveEnvelope so LOD min/max and the zoomed-in path stay identical.
    static_assert(kWaveDbFloor == wa::kWaveEnvDbFloor, "waveform dB floor mismatch");
    return wa::waveDbWarp(x);
}

std::string formatLabel(const wa::AudioFormat& fmt) {
    std::string text = wa::formatAudio(fmt);
    if (fmt.validBits() != fmt.bitsPerSample)
        text += " (" + std::to_string(fmt.validBits()) + " valid)";
    return text;
}

static std::string deviceEnhancementsSuffix(const wa::DeviceInfo& d) {
    if (d.enhancements == wa::EnhancementsState::Enabled)
        return std::string(" [") + wa::ui_text::get(wa::ui_text::kEnhancementsOnShort) + "]";
    if (d.enhancements == wa::EnhancementsState::Disabled)
        return std::string(" [") + wa::ui_text::get(wa::ui_text::kEnhancementsOffShort) + "]";
    return {};
}

static std::string devicePickerLabel(const wa::DeviceInfo& d, bool starredPreview) {
    const char* mark = starredPreview
        ? (d.isDefault ? "* " : "")
        : (d.isDefault ? "* " : "  ");
    return std::string(mark) + wtou(d.name) + deviceEnhancementsSuffix(d);
}

struct StreamFxUi {
    wa::DeviceId* deviceId = nullptr;
    wa::DataFlow* flow = nullptr;
    std::vector<wa::StreamAudioEffect>* effects = nullptr;
    std::string* error = nullptr;
    bool* haveResult = nullptr;
    bool streamsActive = false;
    std::function<void()> onEnhancementsChanged;
};

static wa::DeviceInfo findDeviceAfterSet(wa::DataFlow flow, const wa::DeviceId& id) {
    wa::ComInitGuard com;
    wa::DeviceEnumerator de;
    if (id.empty()) {
        wa::DeviceInfo def{};
        de.defaultDevice(flow, def);
        return def;
    }
    std::vector<wa::DeviceInfo> all;
    if (!de.enumerate(flow, all)) return {};
    for (auto& x : all) {
        if (x.id == id) return x;
    }
    return {};
}

static void applyEndpointEnhancements(const wa::DeviceInfo& d, wa::DataFlow flow,
                                           wa::EnhancementsState want, StreamFxUi& fx) {
    if (fx.error) fx.error->clear();
    if (fx.streamsActive) {
        if (fx.error) *fx.error = wa::refuseEnhancementsWhileStreaming().message;
        return;
    }
    wa::DeviceInfo after{};
    wa::Result r = wa::setEndpointEnhancements(flow, d.id, want, &after);
    if (!r && wa::resultIsAccessDenied(r)) {
        const std::wstring exe = wa::currentExecutablePath();
        if (exe.empty()) {
            if (fx.error) *fx.error = r.message + " (GUI executable path unavailable for elevation)";
            return;
        }
        const auto params = wa::endpoint_enhancements_action::arguments(flow, d.id, want);
        unsigned long exitCode = 1;
        r = wa::runElevatedAndWait(exe, params, &exitCode);
        if (r) after = findDeviceAfterSet(flow, d.id);
    }
    if (!r) {
        if (fx.error) *fx.error = r.message;
        return;
    }
    // Surface mismatch if write returned ok but re-read still shows the old SysFx state.
    if (after.enhancements != wa::EnhancementsState::Unknown && after.enhancements != want) {
        if (fx.error) {
            *fx.error = std::string("Enhancements write ok but device still reports ")
                + wa::enhancementsStateName(after.enhancements)
                + " (want " + wa::enhancementsStateName(want) + "; mix "
                + std::to_string(after.mixFormat.sampleRate) + "/"
                + std::to_string(after.mixFormat.channels) + "ch)";
        }
    }
    if (fx.onEnhancementsChanged) fx.onEnhancementsChanged();
}

static void drawDeviceEnhancementsDetail(const wa::DeviceInfo& d, wa::DataFlow flow,
                                         StreamFxUi fx) {
    // Capture + Render both draw Enable/Disable/Probe with the same ui_text::id()
    // labels; PushID(flow) keeps ImGui IDs unique so Render clicks are not eaten.
    ImGui::PushID(flow == wa::DataFlow::Capture ? "capEpFx" : "renEpFx");
    ImGui::TextDisabled("%s: %s",
        wa::ui_text::get(wa::ui_text::kEnhancementsLabel),
        wa::enhancementsStateName(d.enhancements));
    if (fx.streamsActive) ImGui::BeginDisabled(true);
    if (ImGui::SmallButton(wa::ui_text::id(wa::ui_text::kEnhancementsEnable)))
        applyEndpointEnhancements(d, flow, wa::EnhancementsState::Enabled, fx);
    ImGui::SameLine();
    if (ImGui::SmallButton(wa::ui_text::id(wa::ui_text::kEnhancementsDisable)))
        applyEndpointEnhancements(d, flow, wa::EnhancementsState::Disabled, fx);
    if (fx.streamsActive) {
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s",
            wa::ui_text::get(wa::ui_text::kEnhancementsLockedStreaming));
    }
    if (fx.error && !fx.error->empty()) {
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", fx.error->c_str());
    }
    // Stream effects require an open Shared client (Initialize, no Start). On demand only.
    ImGui::SameLine();
    if (ImGui::SmallButton(wa::ui_text::id(wa::ui_text::kProbeStreamEffects))) {
        *fx.deviceId = d.id;
        *fx.flow = flow;
        *fx.haveResult = true;
        fx.effects->clear();
        fx.error->clear();
        wa::Result r = wa::probeStreamAudioEffects(flow, d.id, *fx.effects);
        if (!r) *fx.error = r.message;
    }
    ImGui::SetItemTooltip("%s\n%s", wa::ui_text::get(wa::ui_text::kProbeEffectsNotice),
        wa::ui_text::get(wa::ui_text::kStreamEffectsNeedWin11));
    if (ImGui::TreeNode(wa::ui_text::id(wa::ui_text::kEffectDetails))) {
        ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kProbeEffectsNotice));
        if (!d.enhancementOptions.empty()) {
            ImGui::TextDisabled("%s:", wa::ui_text::get(wa::ui_text::kEnhancementOptions));
            for (const auto& o : d.enhancementOptions) {
                // Abbreviation only; hover shows full name + CLSID/value.
                ImGui::BulletText("%s", o.id.c_str());
                ImGui::SetItemTooltip("%s\n%s", o.name.c_str(), o.value.c_str());
            }
        }

        if (*fx.haveResult && *fx.deviceId == d.id && *fx.flow == flow) {
            if (!fx.error->empty()) {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", fx.error->c_str());
            } else if (fx.effects->empty()) {
                ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kStreamEffectsNone));
            } else {
                for (size_t ei = 0; ei < fx.effects->size(); ++ei) {
                    auto& e = (*fx.effects)[ei];
                    ImGui::BulletText("%s [%s]", e.typeName.c_str(),
                        wa::audioEffectStateName(e.state));
                    if (!e.canSetState) continue;
                    ImGui::SameLine();
                    char onId[96], offId[96];
                    const char* flowTag = (flow == wa::DataFlow::Capture) ? "c" : "r";
                    std::snprintf(onId, sizeof onId, "%s###fxOn%s%zu",
                        wa::ui_text::get(wa::ui_text::kStreamEffectOn), flowTag, ei);
                    std::snprintf(offId, sizeof offId, "%s###fxOff%s%zu",
                        wa::ui_text::get(wa::ui_text::kStreamEffectOff), flowTag, ei);
                    // Copy id: setStreamAudioEffect may replace *fx.effects.
                    const std::string effectId = e.id;
                    bool mutated = false;
                    auto applySet = [&](wa::AudioEffectState next) {
                        fx.error->clear();
                        wa::Result sr = wa::setStreamAudioEffect(
                            flow, d.id, effectId, next, fx.effects);
                        if (!sr) {
                            *fx.error = sr.message;
                        } else {
                            *fx.haveResult = true;
                            *fx.deviceId = d.id;
                            *fx.flow = flow;
                            mutated = true;
                        }
                    };
                    if (ImGui::SmallButton(onId)) applySet(wa::AudioEffectState::On);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(offId)) applySet(wa::AudioEffectState::Off);
                    if (mutated) break;
                }
            }
        } else {
            ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kStreamEffectsHint));
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

} // namespace

void AppUi::refreshMonitorDevices() {
    wa::ComInitGuard com;   // REQUIRED: GUI thread has no COM; DeviceEnumerator needs it
    const wa::DeviceId prevCap = (capDevIdx_ >= 0 && capDevIdx_ < (int)capDevices_.size())
                                     ? capDevices_[(size_t)capDevIdx_].id : L"";
    const wa::DeviceId prevRen = (renderDevIdx_ >= 0 && renderDevIdx_ < (int)renderDevices_.size())
                                     ? renderDevices_[(size_t)renderDevIdx_].id : L"";
    const wa::DeviceId prevLoopback = (loopbackDevIdx_ >= 0 && loopbackDevIdx_ < (int)renderDevices_.size())
                                      ? renderDevices_[(size_t)loopbackDevIdx_].id : L"";
    const wa::DeviceId prevRenderPage =
        (renderPageDevIdx_ >= 0 && renderPageDevIdx_ < (int)renderDevices_.size())
            ? renderDevices_[(size_t)renderPageDevIdx_].id : L"";
    const wa::DeviceId prevAppReference =
        (appLoopbackReferenceIdx_ >= 2 &&
         appLoopbackReferenceIdx_ - 2 < (int)renderDevices_.size())
            ? renderDevices_[static_cast<size_t>(appLoopbackReferenceIdx_ - 2)].id
            : L"";
    capDevices_.clear();
    renderDevices_.clear();
    enumerator_.enumerate(wa::DataFlow::Capture, capDevices_);
    enumerator_.enumerate(wa::DataFlow::Render,  renderDevices_);
    // Keep the previously selected device if it still exists; else the system default; else 0.
    auto pick = [](const std::vector<wa::DeviceInfo>& devs, const wa::DeviceId& prev) -> int {
        if (!prev.empty())
            for (int i = 0; i < (int)devs.size(); ++i)
                if (devs[(size_t)i].id == prev) return i;
        for (int i = 0; i < (int)devs.size(); ++i)
            if (devs[(size_t)i].isDefault) return i;
        return 0;
    };
    capDevIdx_    = pick(capDevices_, prevCap);
    renderDevIdx_ = pick(renderDevices_, prevRen);
    loopbackDevIdx_ = pick(renderDevices_, prevLoopback);
    renderPageDevIdx_ = pick(renderDevices_, prevRenderPage);
    if (appLoopbackReferenceIdx_ >= 2) {
        appLoopbackReferenceIdx_ = 0;
        for (int i = 0; i < (int)renderDevices_.size(); ++i) {
            if (renderDevices_[static_cast<size_t>(i)].id == prevAppReference) {
                appLoopbackReferenceIdx_ = i + 2;
                break;
            }
        }
    }
    loopbackRecipe_.deviceShown = -1;
    appLoopbackRecipe_.deviceShown = -1;
    renderRecipe_.deviceShown = -1;
    monitorDevicesLoaded_ = true;
}

void AppUi::setMainWindow(void* hwnd) { pipelineHwnd_ = hwnd; }

void AppUi::stopAll() {
    pipelineExport_.cancel();
    wa::CallLogExportResult exportResult;
    pipelineExport_.wait(exportResult);
    pipelineWatch_.stop();
    monitor_.stop();
    loopbackTracks_.destroyAll();
    appLoopbackTracks_.destroyAll();
    renderTracks_.destroyAll();
    pipelineEtw_.stop();
    pipelineAttaches_.shutdown();
}

void AppUi::releaseGpu() {
    const auto release = [](VisualState& viz) {
        viz.capSpec.releaseGpu();
        viz.renderSpec.releaseGpu();
        for (auto& channel : viz.capChannelSpecs) channel->releaseGpu();
    };
    release(monitorViz_);
    for (auto& entry : loopbackViz_) release(entry.second);
    for (auto& entry : appLoopbackViz_) release(entry.second);
    for (auto& entry : renderViz_) release(entry.second);
}

void AppUi::refreshApplicationLoopbackSessions() {
    std::vector<wa::AudioSessionProcess> rows;
    wa::Result r = sessionEnumerator_.enumerate(rows);
    if (!r) {
        wa::app_loopback_ui::applyRefreshResult(false, appLoopbackSessions_,
                                                appLoopbackSessionsLoaded_,
                                                appLoopbackSessionIdx_, {});
        logLines_.push_back("application loopback refresh failed: " + r.message);
        return;
    }
    wa::app_loopback_ui::applyRefreshResult(true, appLoopbackSessions_,
                                            appLoopbackSessionsLoaded_,
                                            appLoopbackSessionIdx_,
                                            std::move(rows));
}

void AppUi::refreshPipelineSessions() {
    const bool hadSelection =
        pipelineSelected_ >= 0 && pipelineSelected_ < (int)pipelineSessions_.size();
    wa::LiveSessionView prev;
    if (hadSelection)
        prev = pipelineSessions_[(size_t)pipelineSelected_];

    std::vector<wa::LiveSessionView> rows;
    wa::Result r = liveSessionEnumerator_.enumerate(rows);
    pipelineSessionsLoaded_ = true;
    if (!r) {
        pipelineSessions_.clear();
        pipelineSelected_ = -1;
        pipelineGraph_.clear();
        rebuildPipelineProcessTree();
        logLines_.push_back("pipeline refresh failed: " + r.message);
        return;
    }

    const uint32_t hidePid = pipelineShowSelf_ ? 0u : static_cast<uint32_t>(GetCurrentProcessId());
    wa::shapeLiveSessionList(rows, hidePid);
    pipelineSessions_ = std::move(rows);

    if (hadSelection) {
        const wa::LiveSessionRestore restored = wa::restoreLiveSession(pipelineSessions_, prev);
        pipelineSelected_ = restored.index;
        // Restore miss: stop On-demand attach. A later reappearance stays on-demand.
        if (restored.detach) {
            // Attach is by PID and may outlive the Live session row. Keep hooks;
            // Hook column falls back to the attached-PID set.
        }
    } else {
        pipelineSelected_ = -1;
    }
    if (pipelineSelected_ < 0)
        pipelineProbes_.clear();
    if (!pipelineAudioOnly_) refreshPipelineProcesses();
    rebuildPipelineProcessTree();
    rebuildPipelineGraph();
}

void AppUi::refreshPipelineProcesses() {
    pipelineProcessesTick_ = GetTickCount64();
    std::vector<wa::UserProcessInfo> procs;
    // Failure keeps the previous list; inaccessible processes are skipped inside.
    if (wa::enumerateUserProcesses(procs)) {
        pipelineProcesses_ = std::move(procs);
        rebuildPipelineProcessTree();
    }
}

void AppUi::rebuildPipelineProcessTree() {
    const uint32_t hidePid = pipelineShowSelf_ ? 0u : static_cast<uint32_t>(GetCurrentProcessId());
    pipelineTree_ = wa::groupProcessTreeByName(wa::buildProcessTree(
        pipelineProcesses_, pipelineSessions_, pipelineAudioOnly_, hidePid));
    applyPipelineProcessSort();
}

void AppUi::applyPipelineProcessSort() {
    wa::sortProcessNameGroups(pipelineTree_, pipelineProcessSortCol_, pipelineProcessSortAsc_);
}

void AppUi::rebuildPipelineGraph() {
    if (pipelineSelected_ < 0 || pipelineSelected_ >= (int)pipelineSessions_.size()) {
        pipelineGraph_.clear();
        return;
    }

    const wa::LiveSessionView& session = pipelineSessions_[(size_t)pipelineSelected_];
    wa::EndpointSnapshot snap;
    const wa::DataFlow flow =
        session.flow == wa::PipelineFlow::Capture ? wa::DataFlow::Capture : wa::DataFlow::Render;
    wa::Result r = endpointGraphReader_.snapshot(flow, utow(session.deviceId.c_str()), snap);
    if (!r)
        logLines_.push_back("pipeline endpoint snapshot failed: " + r.message);
    pipelineEndpoint_ = std::move(snap);
    applyPipelineJoin();
}

void AppUi::applyLiveSessionCellPatches() {
    const std::vector<wa::LiveSessionCellPatch> patches = pipelineWatch_.drainCellPatches();
    bool selectedChanged = false;
    for (const auto& patch : patches) {
        const auto index = wa::applyLiveSessionCellPatch(pipelineSessions_, patch);
        selectedChanged = selectedChanged || (index >= 0 && index == pipelineSelected_);
    }
    if (selectedChanged) applyPipelineJoin();
}

void AppUi::applyPipelineJoin() {
    wa::FrameBracket graph(frameProbe_, wa::FrameSlice::GraphHint);
    pipelineJoinStatus_ = {};
    if (pipelineSelected_ < 0 || pipelineSelected_ >= (int)pipelineSessions_.size()) {
        pipelineGraph_.clear();
        return;
    }
    const wa::LiveSessionView& session = pipelineSessions_[(size_t)pipelineSelected_];
    wa::EtwInitializeHint etw;
    if (pipelineEtw_.status() == wa::EtwWatchStatus::Listening)
        etw = wa::matchEtwInitialize(session, pipelineEtw_.snapshot(), wa::etwNowMs());
    pipelineEtwHint_ = etw;
    const wa::ProcessInstance instance{session.processId, session.processCreated};
    const auto& hooked = pipelineCallStore_.initializeHintForInstance(instance, {&frameProbe_,
        [](void* context, wa::CallLogStage stage, bool begin, size_t count) {
            if (stage == wa::CallLogStage::Initialize && begin)
                static_cast<wa::FrameProbe*>(context)->addWork("initialize-contributors", count);
        }});
    pipelineJoinStatus_.etwMatched = etw.present;
    pipelineJoinStatus_.probesPresent = !pipelineProbes_.empty();
    const auto graphSegment = pipelineCallStore_.segment(pipelineCallStore_.latestAttach(instance));
    pipelineJoinStatus_.sessionControlCalls = graphSegment ? graphSegment->count : 0;
    pipelineJoinStatus_.otherPidCalls = pipelineCallStore_.size() -
        pipelineCallStore_.pidCallCount(session.processId) -
        (session.processId != 0 ? pipelineCallStore_.pidCallCount(0) : 0);
    pipelineJoinStatus_.hookedStreamCount = hooked.streamCount;
    pipelineJoinStatus_.primaryStreamId = hooked.primaryStreamId;
    pipelineJoinStatus_.initializeSource = hooked.hint.present ? wa::InitializeFieldSource::Hooked :
        (etw.present ? wa::InitializeFieldSource::Etw : wa::InitializeFieldSource::None);
    const bool rebuilt = pipelineGraph_.update(session, pipelineEndpoint_,
        wa::mergeInitializeHint(etw, hooked.hint), pipelineProbes_);
    frameProbe_.addWork("graph-rebuilds", rebuilt);
}

void AppUi::runPipelineProbe() {
    if (pipelineProbing_) return;
    if (pipelineSelected_ < 0 || pipelineSelected_ >= (int)pipelineSessions_.size())
        return;

    const auto sessionsCopy = pipelineSessions_;
    const int selected = pipelineSelected_;
    const wa::LiveSessionView session = pipelineSessions_[(size_t)pipelineSelected_];
    const wa::DataFlow flow =
        session.flow == wa::PipelineFlow::Capture ? wa::DataFlow::Capture : wa::DataFlow::Render;

    pipelineProbing_ = true;
    std::vector<wa::ProbeSlice> slices;
    wa::Result r = wa::probeEndpointShared(flow, utow(session.deviceId.c_str()), slices);
    pipelineProbing_ = false;

    if (pipelineSessions_.size() != sessionsCopy.size() || pipelineSelected_ != selected) {
        logLines_.push_back("pipeline probe ignored: Live session list changed");
        return;
    }
    if (!r) {
        logLines_.push_back("pipeline probe failed: " + r.message);
        rebuildPipelineGraph();
        return;
    }
    pipelineProbes_ = std::move(slices);
    int failed = 0;
    for (const auto& s : pipelineProbes_)
        if (!s.error.empty()) ++failed;
    logLines_.push_back(failed ? ("pipeline probe finished with " + std::to_string(failed) +
                                  " recipe error(s)")
                               : "pipeline probe completed");
    rebuildPipelineGraph();
}

std::unordered_set<uint32_t> AppUi::pipelineAttachedPidSet() const {
    std::unordered_set<uint32_t> out;
    for (uint32_t pid : pipelineAttaches_.attachedPids()) out.insert(pid);
    return out;
}

wa::PipelineAttachUiState AppUi::pipelineAttachUiFor(const wa::LiveSessionView& row) const {
    return wa::pipeline_attach_ui::hookStatusForRow(row, pipelineAttachedPidSet(),
                                                    pipelineAttachFailByPid_, pipelineAttachUi_);
}

void AppUi::setPipelineAttachUi(const wa::LiveSessionView& row, wa::PipelineAttachUiState state,
                                const std::string& banner) {
    const std::string key = wa::liveSessionAttachKey(row);
    pipelineAttachUi_[key] = state;
    if (state == wa::PipelineAttachUiState::Failed)
        pipelineAttachFailBanner_[key] = banner;
    else
        pipelineAttachFailBanner_.erase(key);
}

void AppUi::runPipelineAttach() {
    uint32_t pid = 0;
    if (!wa::pipeline_attach_ui::parseAttachPid(pipelineAttachPid_, pid)) {
        pipelineAttachFailByPid_[0] = wa::attachBlockText(wa::AttachBlock::PidZero);
        logLines_.push_back("pipeline attach failed: " + pipelineAttachFailByPid_[0]);
        applyPipelineJoin();
        return;
    }
    pipelineAttachFailByPid_.erase(0);
    if (pipelineAttaches_.attached(pid)) {
        logLines_.push_back("pipeline already attached pid=" + std::to_string(pid));
        applyPipelineJoin();
        return;
    }
    std::string deviceId;
    wa::PipelineFlow flow = wa::PipelineFlow::Capture;
    std::string sessionKey;
    wa::pipeline_attach_ui::sessionHintForPid(
        pipelineSessions_, pipelineSelected_, pid, deviceId, flow, sessionKey);
    wa::pipeline_attach_ui::AttachRequest request{pid, deviceId, flow, sessionKey};
    if (pipelineSelected_ >= 0 && pipelineSelected_ < static_cast<int>(pipelineSessions_.size())) {
        const auto& selected = pipelineSessions_[static_cast<size_t>(pipelineSelected_)];
        if (selected.processId == pid) request.instance = {pid, selected.processCreated};
    }
    if (!request.instance) {
        const auto identity = wa::queryProcessInstance(pid, request.instance);
        if (!identity) {
            pipelineAttachFailByPid_[pid] = identity.message;
            return;
        }
    }
    attemptPipelineAttach(request);
}

void AppUi::attemptPipelineAttach(const wa::pipeline_attach_ui::AttachRequest& request) {
    const auto pid = request.pid;
    const auto& deviceId = request.deviceId;
    const auto flow = request.flow;
    const auto initial = pipelineInitialMethods_.find(monitorInstanceKey(request.instance));
    wa::Result r = pipelineAttaches_.start(pid, deviceId, flow, request.instance,
        initial == pipelineInitialMethods_.end() ? UINT32_MAX : initial->second.methods);
    if (r) {
        pipelinePreparing_ = request;
        logLines_.push_back("pipeline preparing attach pid=" + std::to_string(pid));
        return;
    }
    pipelineAttachFailByPid_[pid] = r.message;
    if (!request.sessionKey.empty()) {
        pipelineAttachUi_[request.sessionKey] = wa::PipelineAttachUiState::Failed;
        pipelineAttachFailBanner_[request.sessionKey] = r.message;
    }
    logLines_.push_back("pipeline attach failed pid=" + std::to_string(pid) + ": " + r.message);
    applyPipelineJoin();
}

void AppUi::pollPipelineAttach(bool forExport) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::vector<wa::PipelineAttachController::Batch> batches;
    // Command completion is interactive feedback, independent of the paced
    // high-frequency record/statistics presentation. Preserve event order.
    for (auto& event : pipelineAttaches_.pollControlFeedback()) {
        wa::PipelineAttachController::Batch feedback;
        feedback.events.push_back(std::move(event));
        batches.push_back(std::move(feedback));
        frameProbe_.addWork("control-feedback", 1);
    }
    if (forExport || pipelinePresentationRefresh_.take(static_cast<uint64_t>(now))) {
        frameProbe_.addWork(forExport ? "export-presentation-handoff" : "presentation-refresh", 1);
        if (!forExport) frameProbe_.addWork("presentation-refresh-steady-ms", static_cast<uint64_t>(now));
        wa::FrameBracket receive(frameProbe_, wa::FrameSlice::CallReceive);
        auto received = pipelineAttaches_.poll();
        batches.insert(batches.end(), std::make_move_iterator(received.begin()), std::make_move_iterator(received.end()));
    }
    for (auto& batch : batches) {
        for (const auto& event : batch.events) {
            if (event.kind == wa::PipelineAttachController::EventKind::Cleared ||
                event.kind == wa::PipelineAttachController::EventKind::ClearFailed) {
                const auto id = pipelineCallStore_.attachForToken(event.token);
                auto& state = pipelineClearStates_[id];
                state.pending = false;
                state.failure = event.result ? "" : event.result.message;
                if (event.result && pipelineCallSelected_.attach == id) {
                    pipelineCallSelected_ = {};
                    pipelinePumpSelectionExpired_ = false;
                }
                continue;
            }
            if (event.kind == wa::PipelineAttachController::EventKind::Configured ||
                event.kind == wa::PipelineAttachController::EventKind::ConfigurationFailed) {
                const auto id = pipelineCallStore_.attachForToken(event.token);
                auto& draft = pipelineMethodDrafts_[id];
                draft.pending = false;
                draft.failure = event.result ? "" : event.result.message;
                continue;
            }
            if (event.kind == wa::PipelineAttachController::EventKind::Detached) continue;
            wa::pipeline_attach_ui::AttachRequest request;
            if (pipelinePreparing_ && pipelinePreparing_->pid == event.pid) {
                request = std::move(*pipelinePreparing_);
                pipelinePreparing_.reset();
            }
            if (event.kind == wa::PipelineAttachController::EventKind::Attached) {
                pipelineInitialMethods_.erase(monitorInstanceKey(event.instance));
                pipelineAttachFailByPid_.erase(event.pid);
                if (!request.sessionKey.empty()) {
                    pipelineAttachUi_[request.sessionKey] = wa::PipelineAttachUiState::Attached;
                    pipelineAttachFailBanner_.erase(request.sessionKey);
                }
                logLines_.push_back("pipeline attached pid=" + std::to_string(event.pid));
            } else {
                pipelineAttachFailByPid_[event.pid] = event.result.message;
                if (!request.sessionKey.empty()) {
                    pipelineAttachUi_[request.sessionKey] = wa::PipelineAttachUiState::Failed;
                    pipelineAttachFailBanner_[request.sessionKey] = event.result.message;
                }
                logLines_.push_back("pipeline attach failed pid=" + std::to_string(event.pid) + ": " + event.result.message);
            }
        }
        auto events = batch.events;
        {
            wa::FrameBracket receive(frameProbe_, wa::FrameSlice::CallReceive);
            frameProbe_.addWork("control-received", batch.calls.size());
            for (const auto& pump : batch.pump) {
                frameProbe_.addWork("pump-received", pump.second.size(), pump.first);
                if (!pump.second.empty()) frameProbe_.addWork("receiver-pump-upper-sequence",
                    pump.second.back().sequence, pump.first);
            }
            for (const auto& transport : batch.transport)
                frameProbe_.addWork("receiver-snapshot-sequence", transport.status.snapshotSequence, transport.token);
            pipelineSegmentSelection_.apply(pipelineCallStore_, std::move(batch));
        }
        for (const auto& event : events) {
            if (event.kind != wa::PipelineAttachController::EventKind::Detached) continue;
            for (const auto& row : pipelineSessions_)
                if (row.processId == event.pid)
                    setPipelineAttachUi(row, wa::PipelineAttachUiState::Detached);
            logLines_.push_back("pipeline detached pid=" + std::to_string(event.pid));
        }
        applyPipelineJoin();
    }
}

void AppUi::runPipelineDetach(uint32_t pid) {
    if (!pid) wa::pipeline_attach_ui::parseAttachPid(pipelineAttachPid_, pid);
    if (pid == 0 && pipelineSelected_ >= 0 && pipelineSelected_ < (int)pipelineSessions_.size())
        pid = pipelineSessions_[(size_t)pipelineSelected_].processId;
    if (pid == 0) return;
    if (pipelineSelected_ >= 0 && pipelineSelected_ < (int)pipelineSessions_.size() &&
        pipelineSessions_[(size_t)pipelineSelected_].processId == pid) {
        setPipelineAttachUi(pipelineSessions_[(size_t)pipelineSelected_],
                            wa::PipelineAttachUiState::Detached);
    }
    if (!pipelineAttaches_.stop(pid)) {
        logLines_.push_back("pipeline detach command rejected: queue full");
        return;
    }
    pipelineAttachFailByPid_.erase(pid);
    logLines_.push_back("pipeline detach requested pid=" + std::to_string(pid));
    applyPipelineJoin();
}

void AppUi::runPipelineDetachAll() {
    const auto pids = pipelineAttaches_.attachedPids();
    if (!pipelineAttaches_.stopAll()) {
        logLines_.push_back("pipeline detach-all command rejected: queue full");
        return;
    }
    pipelineAttachFailByPid_.clear();
    for (auto& kv : pipelineAttachUi_) kv.second = wa::PipelineAttachUiState::Detached;
    pipelineAttachFailBanner_.clear();
    logLines_.push_back("pipeline detach-all requested (" + std::to_string(pids.size()) + " pid(s))");
    applyPipelineJoin();
}

bool AppUi::savePipelineCallLog(const std::wstring& destination) {
    wa::FrameBracket saving(frameProbe_, wa::FrameSlice::ExportStart);
    if (pipelineExport_.busy()) {
        logLines_.push_back("pipeline save calls: export is busy");
        return false;
    }
    const auto selected = pipelineSegmentSelection_.selected();
    if (!pipelineCallStore_.segment(selected)) {
        logLines_.push_back("pipeline save calls: no selected segment");
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto snapshot = pipelineSegmentSelection_.freezeExport(pipelineCallStore_, now, [&] {
        pollPipelineAttach(true);
    });
    const auto count = snapshot.records.size();
    frameProbe_.addWork("export-reference-bytes", snapshot.records.capacity() * sizeof(wa::CallRecordRef));
    const auto r = pipelineExport_.start(std::move(snapshot), destination);
    if (!r) {
        logLines_.push_back("pipeline save calls failed: " + r.message);
        return false;
    }
    pipelineExportReveal_ = destination.empty();
    logLines_.push_back("pipeline saving calls: " + std::to_string(count));
    return true;
}

void AppUi::pollBackground(bool notify) {
    wa::CallLogExportResult result;
    if (pipelineExport_.take(result)) pipelineExportCompleted_ = std::move(result);
    if (!notify || !pipelineExportCompleted_) return;
    result = std::move(*pipelineExportCompleted_);
    pipelineExportCompleted_.reset();
    exportElapsedUs_ = result.elapsedUs;
    exportWritten_ = result.recordsWritten;
    exportMetricsPending_ = true;
    if (result.result) {
        logLines_.push_back("pipeline saved calls: " + wtou(result.path) +
                            " (" + std::to_string(result.recordsWritten) + ")");
        if (pipelineExportReveal_ && !exportExit_.requested()) wa::dump_ui::revealDumpFile(result.path);
    } else {
        logLines_.push_back("pipeline save calls incomplete: " + result.result.message);
    }
}

void AppUi::drawExportCloseDialog() {
    if (!exportExit_.requested() || !pipelineExport_.busy()) return;
    const auto* title = wa::ui_text::id(wa::ui_text::kPipelineExportExit);
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineExportExitHint));
    if (exportExit_.prompt() && ImGui::Button(wa::ui_text::get(wa::ui_text::kPipelineExportWait)))
        exportExit_.choose(false);
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::get(wa::ui_text::kPipelineExportCancelExit))) {
        pipelineExport_.cancel();
        exportExit_.choose(true);
    }
    ImGui::EndPopup();
}

void AppUi::setPipelineRecording(bool on) {
    pipelineCallStore_.setRecording(on);
}

void AppUi::receivePipelineCalls(std::vector<wa::HookedCall> calls) {
    if (calls.empty()) return;
    {
        wa::FrameBracket receive(frameProbe_, wa::FrameSlice::CallReceive);
        frameProbe_.addWork("control-received", calls.size());
        pipelineCallStore_.append(std::move(calls));
    }
    applyPipelineJoin();
}

void AppUi::pushLog(int /*level*/, const std::string& line) {
    // Preference Warns stay in the file sink. A failed language save must not
    // add a log-panel line or a dialog; the in-memory choice still stands.
    if (line.find("LogRegionPrefs::") != std::string::npos ||
        line.find("GuiLanguage::save") != std::string::npos)
        return;
    std::lock_guard<std::mutex> lk(logMutex_);
    pendingLog_.push_back(line);
}

void AppUi::recomputeDefaultFormat() {
    wa::ComInitGuard com;
    capsCache_ = wa::DeviceCapabilities{};
    wa::DeviceId capId = (!capDevices_.empty() && capDevIdx_ >= 0 && capDevIdx_ < (int)capDevices_.size())
                         ? capDevices_[(size_t)capDevIdx_].id : L"";
    enumerator_.queryCapabilities(wa::DataFlow::Capture, capId, capsCache_);

    wa::BackendKind kind = (backendIdx_ == 1) ? wa::BackendKind::WasapiExclusive
                                              : wa::BackendKind::WasapiShared;
    auto exclPred = [&](const wa::AudioFormat& fmt) -> bool {
        for (const auto& fs : capsCache_.matrix)
            if (fs.fmt == fmt) return wa::isSupported(fs.exclusive);
        return false;
    };
    const wa::AudioFormat* devFmt = capsCache_.hasDevice ? &capsCache_.deviceFormat : nullptr;
    selectedFmt_ = wa::chooseDefaultFormat(kind, capsCache_.mixFormat, devFmt,
                                           wa::defaultExclusiveCaptureCandidates(), exclPred);
    // Shared: pass nullptr to engine (GetMixFormat); Exclusive: pass selectedFmt_ so engine uses
    // the same format we already probed and display, making "shown == used" exact.
    haveFmt_      = (kind == wa::BackendKind::WasapiExclusive);
    fmtChoiceIdx_ = 0;
}

void AppUi::drawFormatRegion() {
    if (backendIdx_ != fmtBackendShown_ || capDevIdx_ != capDevShown_) {
        recomputeDefaultFormat();
        fmtBackendShown_ = backendIdx_;
        capDevShown_     = capDevIdx_;
    }

    // Display current effective format
    const wa::AudioFormat& f = selectedFmt_;
    ImGui::Text("%s: %u/%u/%u%s%s",
                wa::ui_text::get(wa::ui_text::kFormat),
                f.sampleRate, (unsigned)f.bitsPerSample, (unsigned)f.channels,
                f.isFloat ? "f" : "",
                haveFmt_ ? "" : wa::ui_text::get(wa::ui_text::kFormatDefault));

    // Build filtered candidate list for the current backend mode
    const bool isExclusive = (backendIdx_ == 1);
    std::vector<wa::AudioFormat> okFmts;
    for (const auto& fs : capsCache_.matrix)
        if (wa::isSupported(isExclusive ? fs.exclusive : fs.shared))
            okFmts.push_back(fs.fmt);
    const int nOk = (int)okFmts.size();

    // Safety clamp. Combo layout: 0=System default, 1..nOk=ok candidates.
    // fmtChoiceIdx_ == -1 means a custom format was applied (not a combo item).
    if (fmtChoiceIdx_ < -1 || fmtChoiceIdx_ > nOk) fmtChoiceIdx_ = 0;

    // Combo preview
    auto fmtStr = [](const wa::AudioFormat& fmt) -> std::string {
        std::string s = std::to_string(fmt.sampleRate) + "/" +
                        std::to_string(fmt.bitsPerSample) + "/" +
                        std::to_string(fmt.channels);
        if (fmt.isFloat) s += "f";
        return s;
    };
    const char* systemDefault = wa::ui_text::get(wa::ui_text::kSystemDefault);
    const std::string preview = (fmtChoiceIdx_ == 0) ? std::string(systemDefault)
                              : (fmtChoiceIdx_ >= 1) ? fmtStr(okFmts[(size_t)(fmtChoiceIdx_ - 1)])
                              : fmtStr(selectedFmt_);   // custom (idx == -1)
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##fmtCombo", preview.c_str())) {
        // Item 0: System default — recomputes selectedFmt_ and haveFmt_ from device/backend
        const std::string defItem = std::string(systemDefault) + "###0";
        if (ImGui::Selectable(defItem.c_str(), fmtChoiceIdx_ == 0)) {
            fmtChoiceIdx_ = 0;
            recomputeDefaultFormat();
        }
        // Items 1..nOk: ok candidates for current backend
        for (int i = 0; i < nOk; ++i) {
            const std::string label = fmtStr(okFmts[(size_t)i]) + "##" + std::to_string(i + 1);
            if (ImGui::Selectable(label.c_str(), fmtChoiceIdx_ == i + 1)) {
                fmtChoiceIdx_ = i + 1;
                selectedFmt_  = okFmts[(size_t)i];
                haveFmt_      = true;
            }
        }
        ImGui::EndCombo();
    }

    // Custom format input — always available; type e.g. 48000/16/2 and click Apply (overrides the combo).
    ImGui::SetNextItemWidth(-60.0f);
    ImGui::InputText("##fmtInput", fmtCustom_, sizeof(fmtCustom_));
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kApply))) {
        formatErrors_["monitor"].clear();
        wa::AudioFormat parsed{};
        if (wa::parseFormatSpec(std::string(fmtCustom_), parsed)) {
            selectedFmt_  = parsed;
            haveFmt_      = true;
            fmtChoiceIdx_ = -1;   // custom, not a combo item
        } else {
            formatErrors_["monitor"] = wa::ui_text::get(wa::ui_text::kInvalidFormatInput);
            logLines_.push_back("invalid format");
        }
    }
    if (!formatErrors_["monitor"].empty())
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", formatErrors_["monitor"].c_str());
}

static void fillCatalogLookup(const char* const* src, const char** dst, int n) {
    for (int i = 0; i < n; ++i) dst[i] = wa::ui_text::get(src[i]);
}

static void drawCaptureStreamParams(wa::StreamParams& p) {
    bool capProps = p.clientProperties.enabled;
    if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kAdvancedSetClientProperties), &capProps))
        p.clientProperties.enabled = capProps;
    ImGui::BeginDisabled(!p.clientProperties.enabled);
    int v = (int)p.clientProperties.category;
    const char* categories[wa::ui_text::kAudioCategoryCount];
    fillCatalogLookup(wa::ui_text::kAudioCategoryLabels, categories, wa::ui_text::kAudioCategoryCount);
    if (ImGui::Combo(wa::ui_text::id(wa::ui_text::kCategory), &v, categories,
                     wa::ui_text::kAudioCategoryCount))
        p.clientProperties.category = (wa::AudioCategory)v;
    bool capOff = p.clientProperties.offload;
    if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kAdvancedHardwareOffload), &capOff))
        p.clientProperties.offload = capOff;
    v = (int)p.clientProperties.option;
    const char* streamOptions[wa::ui_text::kAdvancedStreamOptionCount];
    fillCatalogLookup(wa::ui_text::kAdvancedStreamOptions, streamOptions,
                 wa::ui_text::kAdvancedStreamOptionCount);
    if (ImGui::Combo(wa::ui_text::id(wa::ui_text::kStreamOptionCaption), &v, streamOptions,
                     wa::ui_text::kAdvancedStreamOptionCount))
        p.clientProperties.option = (wa::StreamOption)v;
    ImGui::EndDisabled();
    v = (int)p.bufferMs;
    if (ImGui::InputInt(wa::ui_text::id(wa::ui_text::kBufferMs), &v))
        p.bufferMs = (uint32_t)std::clamp(v, 0, 2000);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kResetToSystemDefaults))) p = wa::StreamParams{};
}

void AppUi::recomputeLoopbackFormat() {
    wa::ComInitGuard com;
    wa::DeviceCapabilities caps;
    wa::DeviceId id = (!renderDevices_.empty() && loopbackDevIdx_ >= 0
                       && loopbackDevIdx_ < (int)renderDevices_.size())
                          ? renderDevices_[(size_t)loopbackDevIdx_].id
                          : L"";
    wa::Result result = enumerator_.queryCapabilities(wa::DataFlow::Render, id, caps);
    if (!result) {
        logLines_.push_back("system loopback capabilities error: " + result.message);
        caps = wa::DeviceCapabilities{};
    }
    wa::create_recipe::updateFormatDevice(loopbackRecipe_, loopbackDevIdx_, id, caps,
                                          caps.mixFormat);
}

void AppUi::recomputeApplicationLoopbackFormat() {
    wa::DeviceCapabilities caps;
    wa::DeviceId id;
    if (appLoopbackReferenceIdx_ == 0) {
        wa::create_recipe::updateFormatDevice(appLoopbackRecipe_, 0, id, caps,
                                              wa::AudioFormat{});
        return;
    }
    if (appLoopbackReferenceIdx_ >= 2) {
        const int deviceIdx = appLoopbackReferenceIdx_ - 2;
        if (deviceIdx < 0 || deviceIdx >= (int)renderDevices_.size()) {
            appLoopbackReferenceIdx_ = 0;
            wa::create_recipe::updateFormatDevice(appLoopbackRecipe_, 0, id, caps,
                                                  wa::AudioFormat{});
            return;
        }
        id = renderDevices_[static_cast<size_t>(deviceIdx)].id;
    }

    wa::ComInitGuard com;
    wa::Result result = enumerator_.queryCapabilities(wa::DataFlow::Render, id, caps);
    if (!result) {
        logLines_.push_back("application loopback format reference error: " + result.message);
        caps = wa::DeviceCapabilities{};
    }
    wa::create_recipe::updateFormatDevice(appLoopbackRecipe_, appLoopbackReferenceIdx_, id,
                                          caps, wa::AudioFormat{});
}

static void drawOperationError(const std::string& error) {
    if (error.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.55f, 1.0f));
    ImGui::TextWrapped("%s", error.c_str());
    ImGui::PopStyleColor();
}

static void prepareAudioModal(float width, float height) {
    const ImVec2 available = ImGui::GetMainViewport()->WorkSize;
    const ImVec2 maximum(std::max(260.0f, available.x - 40.0f),
                         std::max(240.0f, available.y - 40.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(260, 240), maximum);
    ImGui::SetNextWindowSize(ImVec2(std::min(width, maximum.x), std::min(height, maximum.y)),
                             ImGuiCond_Appearing);
}

void AppUi::drawFormatRecipe(wa::create_recipe::FormatState& st,
                             const std::vector<wa::FormatSupport>& candidates,
                             const wa::AudioFormat& defaultDisplay,
                             bool hasDefaultDisplay, const char* defaultLabel,
                             const char* comboId, wa::StreamParams& params) {
    ImGui::PushID(comboId);
    const int nOk = (int)candidates.size();
    if (st.choiceIdx < -1 || st.choiceIdx > nOk) st.choiceIdx = 0;

    const char* shownDefault = wa::ui_text::get(defaultLabel);
    const char* formatCaption = wa::ui_text::get(wa::ui_text::kFormat);
    if (st.haveRequested) {
        ImGui::Text("%s: %s", formatCaption, formatLabel(st.selected).c_str());
    } else if (hasDefaultDisplay) {
        ImGui::Text("%s: %s (%s)", formatCaption, formatLabel(defaultDisplay).c_str(),
                    shownDefault);
    } else {
        ImGui::Text("%s: %s", formatCaption, shownDefault);
    }

    const std::string preview = (st.choiceIdx == 0) ? std::string(shownDefault)
                              : (st.choiceIdx >= 1)
                                    ? formatLabel(candidates[(size_t)(st.choiceIdx - 1)].fmt)
                                    : formatLabel(st.selected);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##fmtCombo", preview.c_str())) {
        const std::string defLabel = std::string(shownDefault) + "###0";
        if (ImGui::Selectable(defLabel.c_str(), st.choiceIdx == 0))
            wa::create_recipe::selectDefault(st, defaultDisplay);
        for (int i = 0; i < nOk; ++i) {
            const auto& choice = candidates[static_cast<size_t>(i)];
            const std::string label = formatLabel(choice.fmt) + "  [" +
                wa::ui_text::formatOriginsLabel(choice.origins) + "; " +
                wa::ui_text::supportLabel(choice.shared) +
                "]##" + std::to_string(i + 1);
            if (ImGui::Selectable(label.c_str(), st.choiceIdx == i + 1))
                wa::create_recipe::selectCandidate(st, choice.fmt, i + 1);
        }
        ImGui::EndCombo();
    }

    ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kCustomFormatHint));
    const float applyWidth = ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kApply)).x
                           + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-applyWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##fmtInput", st.custom, sizeof(st.custom));
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kApply))) {
        auto& error = formatErrors_[comboId];
        error.clear();
        if (!wa::create_recipe::applyCustom(st, st.custom, &error))
            logLines_.push_back(error);
    }
    drawOperationError(formatErrors_[comboId]);
    drawCaptureOptions(params);
    ImGui::PopID();
}

static void drawParamsSummary(const wa::StreamParams& p) {
    if (p.isDefault()) return;
    std::string detail;
    if (p.bufferMs) detail += "Buffer " + std::to_string(p.bufferMs) + " ms | ";
    if (p.clientProperties.enabled) {
        detail += std::string(wa::ui_text::get(wa::ui_text::kAudioCategoryLabels[static_cast<int>(p.clientProperties.category)]));
        detail += " | ";
        detail += wa::ui_text::get(wa::ui_text::kAdvancedStreamOptions[static_cast<int>(p.clientProperties.option)]);
        if (p.clientProperties.offload) detail += " | Offload";
    }
    if (p.ducking == wa::DuckingMode::OptOut) detail += " | Ducking opt-out";
    if (p.autoConvert != wa::AutoConvert::Default) detail += " | AutoConvert=" + std::to_string(static_cast<int>(p.autoConvert));
    ImGui::TextWrapped("%s: %s", wa::ui_text::get(wa::ui_text::kNonDefaultParams), detail.c_str());
}

void AppUi::drawCaptureOptions(wa::StreamParams& params) {
    drawParamsSummary(params);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kOptions)))
        ImGui::OpenPopup(wa::ui_text::id(wa::ui_text::kCaptureOptionsPopup));
    prepareAudioModal(560, 480);
    if (!ImGui::BeginPopupModal(wa::ui_text::id(wa::ui_text::kCaptureOptionsPopup), nullptr,
                                ImGuiWindowFlags_None))
        return;
    ImGui::BeginChild("optionsBody", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2), false);
    ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kAdvancedOptionsHelp));
    ImGui::Separator();
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kAdvancedCaptureSection));
    ImGui::PushItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x * 0.42f));
    drawCaptureStreamParams(params);
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kClose), ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Draw the Y-axis unit fixed at the plot's top-left (at the top of the tick numbers), with a
// translucent backing so it stays readable over heatmap pixels. Call between the plot items and
// EndPlot (setup must be locked).
static void drawYUnitLabel(const char* unit, bool rightSide = false) {
    const ImVec2 p  = ImPlot::GetPlotPos();
    const ImVec2 ts = ImGui::CalcTextSize(unit);
    ImDrawList*  dl = ImPlot::GetPlotDrawList();
    const ImVec2 a(rightSide ? (p.x + ImPlot::GetPlotSize().x - ts.x - 5.0f) : (p.x + 5.0f),
                   p.y + 4.0f);
    ImPlot::PushPlotClipRect();
    dl->AddRectFilled(ImVec2(a.x - 3.0f, a.y - 2.0f), ImVec2(a.x + ts.x + 3.0f, a.y + ts.y + 2.0f),
                      IM_COL32(20, 20, 20, 130), 3.0f);
    dl->AddText(a, IM_COL32(230, 230, 230, 230), unit);
    ImPlot::PopPlotClipRect();
}

const char* AppUi::chartTitle(int id) {
    switch (id) {
    case 0: return wa::ui_text::get(wa::ui_text::kCaptureWaveformSpectrogram);
    case 1: return wa::ui_text::get(wa::ui_text::kRenderWaveformSpectrogramDelayed);
    default: return "";
    }
}

void AppUi::resetVisuals(VisualState& viz) {
    viz.capWave.clear();
    viz.renderWave.clear();
    viz.capChannelWaves.clear();
    viz.waveSr = 0;
    viz.waveN = 0;
    viz.capWaveChannels = 0;
    viz.xLink0 = 0.0;
    viz.xLink1 = 0.0;
    viz.envX.clear();
    viz.envMin.clear();
    viz.envMax.clear();
    viz.capEnvPyramid = {};
    viz.renderEnvPyramid = {};
    viz.capEnvPyramids.clear();
    viz.workCap.clear();
    viz.workRender.clear();
    viz.specWin.clear();
    viz.capSpecWindows.clear();
    viz.magCap.clear();
    viz.magRender.clear();
    viz.nextCapEnd = 0;
    viz.nextRenderEnd = 0;
    viz.capWaveState.invalidate();
    viz.renderWaveState.invalidate();
    viz.specSr = 0;
    viz.capSpecChannels = 0;
    viz.capSpec.reset();
    viz.renderSpec.reset();
    viz.capChannelSpecs.clear();
    viz.chartsFrozen = false;
    viz.resetYAxes = false;
    std::fill(std::begin(viz.plotHovPrev), std::end(viz.plotHovPrev), false);
}

void AppUi::resetRenderVisuals(VisualState& viz) {
    viz.magRender.clear();
    viz.renderSpec.reset();
    viz.nextRenderEnd = 0;
    viz.renderWaveState.invalidate();
    viz.renderEnvPyramid = {};
    std::fill(viz.renderWave.begin(), viz.renderWave.end(), 0.f);
}

void AppUi::ensureRunningVisuals(const wa::MonitorStatus& status, VisualState& viz,
                                 bool includeRender) {
    const uint32_t sr = status.sampleRate;
    const uint32_t hz = (sr > 0) ? sr : 48000u;
    if (viz.xLink1 <= 0.0)
        viz.xLink1 = (double)(kSpecCols * kFftHop) / (double)hz;

    const bool overallRunning = (status.overall == wa::StreamState::Running && sr > 0);
    if (!overallRunning) return;

    const uint32_t capChannels =
        wa::capture_channel_view::makePlan(status.captureChannels).visibleChannels;
    if (sr != viz.waveSr || capChannels != viz.capWaveChannels) {
        viz.waveSr = sr;
        viz.waveN = (int)(kSpecCols * kFftHop);
        viz.capWave.assign((size_t)viz.waveN, 0.f);
        if (includeRender) viz.renderWave.assign((size_t)viz.waveN, 0.f);
        viz.capChannelWaves.assign((size_t)capChannels,
                                   std::vector<float>((size_t)viz.waveN, 0.f));
        viz.capWaveChannels = capChannels;
        viz.capWaveState.invalidate();
        viz.renderWaveState.invalidate();
        viz.xLink0 = 0.0;
        viz.xLink1 = (double)(kSpecCols * kFftHop) / (double)sr;
    }

    if (sr != viz.specSr || capChannels != viz.capSpecChannels) {
        viz.specSr = sr;
        viz.workCap.resize(kFftWin);
        if (includeRender) viz.workRender.resize(kFftWin);
        viz.specWin.resize(kFftWin);
        viz.capSpec.reset(std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0,
                                                        (double)sr / 2.0, sr));
        if (includeRender) {
            viz.renderSpec.reset(std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0,
                                                               (double)sr / 2.0, sr));
        }
        viz.capChannelSpecs.clear();
        viz.capChannelSpecs.reserve((size_t)capChannels);
        for (uint32_t ch = 0; ch < capChannels; ++ch) {
            auto visual = std::make_unique<wa::SpectrogramVisual>();
            visual->reset(std::make_unique<wa::Spectrogram>(
                kSpecRows, kSpecCols, 20.0, (double)sr / 2.0, sr));
            viz.capChannelSpecs.push_back(std::move(visual));
        }
        viz.capSpecWindows.assign((size_t)capChannels, std::vector<float>(kFftWin, 0.f));
        viz.capSpecChannels = capChannels;
        viz.nextCapEnd = 0;
        if (includeRender) viz.nextRenderEnd = 0;
    } else if (includeRender && !viz.renderSpec) {
        viz.renderSpec.reset(std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0,
                                                           (double)viz.specSr / 2.0,
                                                           viz.specSr));
    }
}

void AppUi::draw() {
    pollPipelineAttach();
    if (exportMetricsPending_) {
        frameProbe_.addWork("export-total-us", exportElapsedUs_);
        frameProbe_.addWork("export-records-written", exportWritten_);
        exportMetricsPending_ = false;
    }
    // Drain lines buffered by the logging pump thread into the panel history (bounded).
    // The bracket covers the lock and the trim. The probe itself only writes its slot.
    {
        wa::FrameBracket logDrain(frameProbe_, wa::FrameSlice::LogDrain);
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            for (auto& l : pendingLog_) logLines_.push_back(std::move(l));
            pendingLog_.clear();
        }
        constexpr size_t kMaxLogLines = 5000;
        if (logLines_.size() > kMaxLogLines) {
            logFirstLine_ += logLines_.size() - kMaxLogLines;
            logLines_.erase(logLines_.begin(),
                            logLines_.begin() + (logLines_.size() - kMaxLogLines));
        }
    }

    applyDumpPick();
    // Volume, mute, and Active patches apply on every frame, including while
    // Pipeline is not the visible tab.
    applyLiveSessionCellPatches();

    // Poll once; detect renderState Running->non-Running to clear stale playback chart data.
    ms_ = monitor_.poll();
    consumeMonitorDumps();
    const int curRenderState = (int)ms_.renderState;
    if (prevRenderState_ == (int)wa::StreamState::Running &&
        curRenderState  != (int)wa::StreamState::Running) {
        // Keep frozen render charts for inspection; wipe only when charts are live.
        if (wa::charts_freeze::shouldResetRenderVisuals(monitorViz_.chartsFrozen))
            resetRenderVisuals(monitorViz_);
    }
    prevRenderState_ = curRenderState;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);

    constexpr ImGuiWindowFlags kWindowFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("WinAudio", nullptr, kWindowFlags);

    const char* langItems[] = {"English", "中文"};
    const float langText = std::max(ImGui::CalcTextSize(langItems[0]).x,
                                    ImGui::CalcTextSize(langItems[1]).x);
    const float langW = langText + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    const float toolGap = ImGui::GetStyle().ItemSpacing.x;
    const float mmsysW = ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kMmsysCpl)).x
                       + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float settingsW = ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kMsSettings)).x
                          + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float toolsW = mmsysW + settingsW + langW + toolGap * 2.0f;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float savedMaxX = window->WorkRect.Max.x;
    const ImVec2 langBarPos = ImGui::GetCursorScreenPos();
    // The tab bar records its rect at Begin. Restore the full width before page content.
    window->WorkRect.Max.x -= toolsW + toolGap;
    refreshActivityCounts();
    const auto pageLabel = [](const char* title, size_t count) {
        std::string label = wa::ui_text::get(title);
        if (count) label += " (" + std::to_string(count) + ")";
        return label + "###" + title;
    };
    const bool pagesOpen = ImGui::BeginTabBar("##pages", ImGuiTabBarFlags_FittingPolicyScroll);
    window->WorkRect.Max.x = savedMaxX;

    if (pagesOpen) {
        bool pageOpen = ImGui::BeginTabItem(pageLabel(wa::ui_text::kMonitorTab,
            ms_.overall == wa::StreamState::Running ? 1 : 0).c_str());
        frameProbe_.pageState("Monitor", ImGui::IsItemClicked(), pageOpen);
        ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kMonitorTabCountHelp));
        if (pageOpen) {
            drawMonitorPage();
            ImGui::EndTabItem();
        }
        pageOpen = ImGui::BeginTabItem(pageLabel(wa::ui_text::kLoopbackTab, activityLoopback_).c_str());
        frameProbe_.pageState("SystemLoopback", ImGui::IsItemClicked(), pageOpen);
        ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kTrackTabCountHelp));
        if (pageOpen) {
            drawLoopbackPage();
            ImGui::EndTabItem();
        }
        pageOpen = ImGui::BeginTabItem(pageLabel(wa::ui_text::kApplicationLoopbackTab, activityAppLoopback_).c_str());
        frameProbe_.pageState("ApplicationLoopback", ImGui::IsItemClicked(), pageOpen);
        ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kTrackTabCountHelp));
        if (pageOpen) {
            drawApplicationLoopbackPage();
            ImGui::EndTabItem();
        }
        pageOpen = ImGui::BeginTabItem(pageLabel(wa::ui_text::kRenderTab, activityRender_).c_str(), nullptr,
#ifdef WA_PERFORMANCE_FIXTURE
                                      ((!ImGui::IsWindowAppearing() || PerformanceFixture::inspector() || PerformanceFixture::monitorDump())
                                          ? ImGuiTabItemFlags_None : ImGuiTabItemFlags_SetSelected)
#else
                                      ImGuiTabItemFlags_None
#endif
        );
        frameProbe_.pageState("Render", ImGui::IsItemClicked(), pageOpen);
        ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kRenderTabCountHelp));
        if (pageOpen) {
            drawRenderPage();
            ImGui::EndTabItem();
        }
        pageOpen = ImGui::BeginTabItem(pageLabel(wa::ui_text::kPipelineTab,
            pipelineAttaches_.attachedPids().size()).c_str(), nullptr,
#ifdef WA_PERFORMANCE_FIXTURE
                                      ((ImGui::IsWindowAppearing() && PerformanceFixture::inspector()) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)
#else
                                      ImGuiTabItemFlags_None
#endif
        );
        frameProbe_.pageState("Pipeline", ImGui::IsItemClicked(), pageOpen);
        ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kPipelineTabCountHelp));
        if (pageOpen) {
            drawPipelinePage();
            ImGui::EndTabItem();
        } else if (!pipelineWatch_.running()) {
            pipelineWatchAttempted_ = false;
        }
        ImGui::EndTabBar();
    }

    const ImVec2 afterTabs = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(savedMaxX - toolsW, langBarPos.y));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kMmsysCpl), ImVec2(mmsysW, 0)))
        openOsSound(wa::os_sound_ui::Button::Mmsys);
    ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kMmsysCplHint));
    ImGui::SameLine(0, toolGap);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kMsSettings), ImVec2(settingsW, 0)))
        openOsSound(wa::os_sound_ui::Button::MsSettings);
    ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kMsSettingsHint));
    ImGui::SameLine(0, toolGap);
    ImGui::SetNextItemWidth(langW);
    int langIdx = wa::ui_text::language() == wa::gui_language::Language::ZhCn ? 1 : 0;
    // Selectable reports the click even when the item is already current, so choosing
    // the display language still stores the override.
    if (ImGui::BeginCombo("##guiLanguage", langItems[langIdx])) {
        for (int i = 0; i < IM_ARRAYSIZE(langItems); ++i) {
            if (!ImGui::Selectable(langItems[i], i == langIdx)) continue;
            const auto chosen = i == 1 ? wa::gui_language::Language::ZhCn
                                       : wa::gui_language::Language::En;
            wa::ui_text::setLanguage(chosen);
            ensureLogPrefsPath();
            if (!wa::log_region_prefs::saveLanguage(logPrefsPath_, chosen, logCollapsed_)) {
                WA_LOG(wa::log::Level::Warn, "GuiLanguage", "save",
                       wtou(logPrefsPath_), "failed");
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetCursorScreenPos(afterTabs);

    drawExportCloseDialog();
    ImGui::End();
}

static void drawWorkspaceSplitter(const char* id, bool vertical, float& fraction, float span, float sign = 1.f) {
    const float thickness = wa::pipeline_split::kSplitterPx;
    ImGui::InvisibleButton(id, vertical ? ImVec2(thickness, std::max(1.f, ImGui::GetContentRegionAvail().y))
                                       : ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x), thickness));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive())
        fraction = wa::pipeline_split::nudgeFrac(fraction,
            sign * (vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y), span, 0.15f, 0.85f);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(vertical ? ImVec2((a.x + b.x) * 0.5f, a.y) : ImVec2(a.x, (a.y + b.y) * 0.5f),
        vertical ? ImVec2((a.x + b.x) * 0.5f, b.y) : ImVec2(b.x, (a.y + b.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Separator));
}

static void continueWorkspaceRow(float width) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
}

void AppUi::refreshActivityCounts() {
    const auto countRunning = [](const auto& tracks) {
        return static_cast<size_t>(std::count_if(tracks.begin(), tracks.end(), [](const auto& t) { return t.state == wa::StreamState::Running; }));
    };
    const uint64_t tick = GetTickCount64();
    if (!activityTick_ || tick - activityTick_ >= 1000) {
        activityLoopback_ = countRunning(loopbackTracks_.poll());
        activityAppLoopback_ = countRunning(appLoopbackTracks_.poll());
        activityRender_ = countRunning(renderTracks_.poll());
        activityTick_ = tick;
    }
}

void AppUi::drawMonitorPage() {
    drawMonitorControls();
    ImGui::Separator();
    const float logH = logRegionHeight();
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - logH - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("monitorTop", ImVec2(0, topHeight), false);
    const float configSpan = std::max(1.f, ImGui::GetContentRegionAvail().x - wa::pipeline_split::kSplitterPx);
    ImGui::BeginChild("left", ImVec2(wa::pipeline_split::firstPaneSize(configSpan, monitorConfigFrac_, 260.f, 360.f), 0), true);
    drawLeftPanel();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    drawWorkspaceSplitter("##configurationSplit", true, monitorConfigFrac_, configSpan);
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("charts", ImVec2(0, 0), true);
    drawChartHost(&monitor_, ms_, monitorViz_, wa::chart_host::Mode::DualReorderable,
                  nullptr, &chartOrder_);
    ImGui::EndChild();
    ImGui::EndChild();

    drawLogRegion("monitorLogRegion", "log");
}

static const char* streamStateText(wa::StreamState state) {
    return wa::ui_text::get(state == wa::StreamState::Running ? wa::ui_text::kRunning
        : state == wa::StreamState::Error ? wa::ui_text::kError : wa::ui_text::kIdle);
}

static void drawTrackViewToolbar(wa::workspace_view::Tracks& view) {
    ImGui::RadioButton(wa::ui_text::id(wa::ui_text::kSelectedTrack), &view.mode, 0);
    continueWorkspaceRow(160.f);
    ImGui::RadioButton(wa::ui_text::id(wa::ui_text::kCompareTracks), &view.mode, 1);
    continueWorkspaceRow(130.f);
    ImGui::RadioButton(wa::ui_text::id(wa::ui_text::kAllTracks), &view.mode, 2);
}

void AppUi::drawStackedCaptureTrackHosts(wa::CaptureTrackList& list,
                                         std::vector<std::pair<wa::TrackId, VisualState>>& viz,
                                         const char* emptyHint) {
    auto tracks = list.poll();
    auto& view = &list == &loopbackTracks_ ? loopbackView_ : appLoopbackView_;
    const auto reconcile = [&] {
        std::vector<uint64_t> live;
        for (const auto& t : tracks) live.push_back(t.id);
        view.reconcile(live);
        viz.erase(std::remove_if(viz.begin(), viz.end(), [&](const auto& e) {
            return std::find(live.begin(), live.end(), e.first) == live.end();
        }), viz.end());
    };
    reconcile();
    ImGui::Text("%s (%zu)", wa::ui_text::get(wa::ui_text::kLoopbackTracks), tracks.size());
    continueWorkspaceRow(160.f);
    ImGui::BeginDisabled(tracks.empty());
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kLoopbackDestroyAll))) {
        std::vector<std::wstring> paths;
        for (const auto& t : tracks) if (t.dumping) paths.push_back(t.dumpPath);
        list.destroyAll();
        for (const auto& path : paths) wa::dump_ui::revealDumpFile(path);
        tracks.clear(); viz.clear(); reconcile();
        logLines_.push_back("capture tracks destroyed");
    }
    ImGui::EndDisabled();
    continueWorkspaceRow(120.f);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kResetLayout))) {
        trackSummaryFrac_ = 0.30f; view.mode = 0;
        if (&list == &loopbackTracks_) loopbackConfigFrac_ = 0.27f;
        else appLoopbackConfigFrac_ = 0.30f;
    }
    if (tracks.empty()) { ImGui::TextWrapped("%s", emptyHint); return; }
    drawTrackViewToolbar(view);
    const float spanY = std::max(1.f, ImGui::GetContentRegionAvail().y - wa::pipeline_split::kSplitterPx);
    const float tableH = wa::pipeline_split::firstPaneSize(spanY, trackSummaryFrac_, 85.f, 160.f);
    std::vector<uint64_t> removed;
    if (ImGui::BeginTable("trackSummary", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(0, tableH))) {
        ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kTrackCaption), ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kSource), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kActualFormat), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kStatus), ImGuiTableColumnFlags_WidthFixed, 95.f);
        ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kTrackActions), ImGuiTableColumnFlags_WidthFixed, 205.f);
        ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kCompare), ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (const auto& t : tracks) {
            ImGui::PushID(static_cast<int>(t.id));
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
            const std::string id = std::to_string(t.id);
            if (ImGui::Selectable(id.c_str(), view.selected == t.id)) view.select(t.id);
            ImGui::TableSetColumnIndex(1);
            if (t.source.kind == wa::CaptureSourceKind::ApplicationLoopback) {
                std::string name;
                for (const auto& row : appLoopbackSessions_) if (row.processId == t.source.processId) { name = wtou(row.processName); break; }
                ImGui::TextWrapped("%s PID %u", name.c_str(), t.source.processId);
                ImGui::TextWrapped("%s", wa::ui_text::get(t.source.processLoopbackMode == wa::ProcessLoopbackMode::ExcludeTree
                    ? wa::ui_text::kExcludeTree : wa::ui_text::kCaptureTree));
            } else {
                std::string name = wtou(t.source.deviceId);
                for (const auto& d : renderDevices_) if (d.id == t.source.deviceId) { name = wtou(d.name); break; }
                ImGui::TextWrapped("%s", name.c_str());
                ImGui::SetItemTooltip("%s", wtou(t.source.deviceId).c_str());
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", t.actualFormat.sampleRate ? formatLabel(t.actualFormat).c_str() : wa::ui_text::get(wa::ui_text::kUnavailableFormat));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(streamStateText(t.state));
            ImGui::Text("xrun %llu", static_cast<unsigned long long>(t.overruns));
            ImGui::ProgressBar(std::max(t.levelL, t.levelR), ImVec2(-1, 5.f), "");
            ImGui::TableSetColumnIndex(4);
            const auto kind = &list == &loopbackTracks_ ? DumpPickKind::Loopback : DumpPickKind::AppLoopback;
            if (drawDumpControls(list, t, kind, "capture track destroyed id=")) removed.push_back(t.id);
            drawOperationError(t.message);
            if (t.dumpError) drawOperationError(t.dumpMessage);
            ImGui::TableSetColumnIndex(5);
            bool compare = view.compared == t.id;
            ImGui::BeginDisabled(view.selected == t.id);
            if (ImGui::Checkbox("##compare", &compare)) { view.compared = compare ? t.id : 0; if (compare) view.mode = 1; }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const auto& t) {
        return std::find(removed.begin(), removed.end(), t.id) != removed.end();
    }), tracks.end());
    reconcile();
    drawWorkspaceSplitter("##trackSummarySplit", false, trackSummaryFrac_, spanY);
    const bool compare = view.mode == 1 && view.compared;
    const float width = compare ? std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f) : 0.f;
    int drawn = 0;
    for (const auto& t : tracks) {
        if (!view.visible(t.id)) continue;
        if (compare && drawn++) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(t.id));
        ImGui::BeginChild("trackCharts", ImVec2(width, view.mode == 2 ? 560.f : 0.f), true);
        ImGui::Text("%s %llu | %s", wa::ui_text::get(wa::ui_text::kTrackCaption), static_cast<unsigned long long>(t.id), streamStateText(t.state));
        if (t.source.kind == wa::CaptureSourceKind::SystemLoopback) {
            continueWorkspaceRow(230.f);
            ImGui::Text("%s: %s", wa::ui_text::get(wa::ui_text::kLoopbackSilentRender), streamStateText(t.silentRenderState));
        }
        auto it = std::find_if(viz.begin(), viz.end(), [&](const auto& e) { return e.first == t.id; });
        if (it == viz.end()) { viz.push_back({t.id, VisualState{}}); it = viz.end() - 1; }
        wa::TrackScopeReader reader(list, t.id);
        it->second.probeSource = t.id;
        drawChartHost(nullptr, statusFromTrack(t), it->second, wa::chart_host::Mode::CaptureOnly, nullptr, nullptr, &reader);
        ImGui::EndChild(); ImGui::PopID();
    }
}

void AppUi::drawLoopbackPage() {
    const float logH = logRegionHeight();
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - logH - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("loopbackTop", ImVec2(0, topHeight), false);
    const float configSpan = std::max(1.f, ImGui::GetContentRegionAvail().x - wa::pipeline_split::kSplitterPx);
    ImGui::BeginChild("loopbackLeft", ImVec2(wa::pipeline_split::firstPaneSize(configSpan, loopbackConfigFrac_, 260.f, 360.f), 0), true);
    drawLoopbackLeftPanel();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    drawWorkspaceSplitter("##configurationSplit", true, loopbackConfigFrac_, configSpan);
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("loopbackCharts", ImVec2(0, 0), true);
    drawStackedCaptureTrackHosts(loopbackTracks_, loopbackViz_, wa::ui_text::get(wa::ui_text::kLoopbackEmptyHint));
    ImGui::EndChild();
    ImGui::EndChild();

    drawLogRegion("loopbackLogRegion", "loopbackLog");
}

void AppUi::drawApplicationLoopbackPage() {
    const float logH = logRegionHeight();
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - logH - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("appLoopbackTop", ImVec2(0, topHeight), false);
    const float configSpan = std::max(1.f, ImGui::GetContentRegionAvail().x - wa::pipeline_split::kSplitterPx);
    ImGui::BeginChild("appLoopbackLeft", ImVec2(wa::pipeline_split::firstPaneSize(configSpan, appLoopbackConfigFrac_, 260.f, 360.f), 0), true);
    drawApplicationLoopbackLeftPanel();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    drawWorkspaceSplitter("##configurationSplit", true, appLoopbackConfigFrac_, configSpan);
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("appLoopbackCharts", ImVec2(0, 0), true);
    drawStackedCaptureTrackHosts(appLoopbackTracks_, appLoopbackViz_,
                                 wa::ui_text::get(wa::ui_text::kApplicationLoopbackEmptyHint));
    ImGui::EndChild();
    ImGui::EndChild();

    drawLogRegion("appLoopbackLogRegion", "appLoopbackLog");
}

namespace {
ImVec4 kindColor(wa::ObservationKind kind) {
    switch (kind) {
        case wa::ObservationKind::Observed: return ImVec4(0.45f, 0.90f, 0.55f, 1.f);
        case wa::ObservationKind::Probed:   return ImVec4(0.95f, 0.85f, 0.35f, 1.f);
        case wa::ObservationKind::Inferred: return ImVec4(0.50f, 0.75f, 0.95f, 1.f);
        case wa::ObservationKind::Skipped:  return ImVec4(0.62f, 0.62f, 0.62f, 1.f);
        case wa::ObservationKind::Unknown:  return ImVec4(0.90f, 0.55f, 0.55f, 1.f);
    }
    return ImVec4(1.f, 1.f, 1.f, 1.f);
}
}  // namespace


void AppUi::drawPipelinePage() {
#ifdef WA_PERFORMANCE_FIXTURE
    PerformanceFixture::prepareInspector(*this);
    if (!PerformanceFixture::inspector() || PerformanceFixture::liveInspector()) {
#endif
    if (!pipelineEtwStarted_) {
        pipelineEtwStarted_ = true;
        wa::Result etw = pipelineEtw_.start();
        if (!etw)
            logLines_.push_back("pipeline ETW unavailable: " + etw.message);
    }
    if (!pipelineWatchAttempted_) {
        pipelineWatchAttempted_ = true;
        if (pipelineHwnd_) {
            wa::Result watch = pipelineWatch_.start(pipelineHwnd_);
            if (!watch)
                logLines_.push_back("pipeline live session watch unavailable: " + watch.message);
        }
    }
    // Coalesced enumerate runs only while Pipeline is drawn. Other tabs leave
    // the sticky dirty bit set and leave the session subscriptions up.
    if (!pipelineSessionsLoaded_ || pipelineWatch_.consumeDirty())
        refreshPipelineSessions();
    if (pipelineSelected_ >= 0 && pipelineEtw_.status() == wa::EtwWatchStatus::Listening) {
        const wa::LiveSessionView& session = pipelineSessions_[(size_t)pipelineSelected_];
        const wa::EtwInitializeHint hint =
            wa::matchEtwInitialize(session, pipelineEtw_.snapshot(), wa::etwNowMs());
        if (!wa::initializeHintEqual(hint, pipelineEtwHint_))
            applyPipelineJoin();
    }
#ifdef WA_PERFORMANCE_FIXTURE
    }
#endif
    frameProbe_.addWork("control-overrun-total", pipelineAttaches_.droppedControlCalls());
    frameProbe_.addWork("pump-xrun-total", pipelineAttaches_.pumpXrunsSum());
    pipelineCallStore_.setPumpVisible(pipelinePump_);
    if (pipelineCallSelected_ && !pipelineCallStore_.find(pipelineCallSelected_)) {
        pipelinePumpSelectionExpired_ = pipelineCallSelected_.lane == wa::CallLane::Pump;
        pipelineCallSelected_ = {};
    }


    drawPipelineControls();
    const float logH = logRegionHeight();
    const float contentH = std::max(120.0f, ImGui::GetContentRegionAvail().y - logH - ImGui::GetStyle().ItemSpacing.y);
    const int focus = pipelineFocusedPane_;
    ImGui::BeginChild("pipelineContent", ImVec2(0, contentH), false);
    const float split = wa::pipeline_split::kSplitterPx;
    if (focus == 0) {
        const float usableX = std::max(1.f, ImGui::GetContentRegionAvail().x - split);
        const float leftW = wa::pipeline_split::firstPaneSize(usableX, pipelineTargetsFrac_, 240.f, 420.f);
        ImGui::BeginChild("pipelineSources", ImVec2(leftW, 0), false);
        const float usableY = std::max(1.f, ImGui::GetContentRegionAvail().y - split);
        const float targetsH = wa::pipeline_split::firstPaneSize(usableY, pipelineTargetsHeightFrac_, 160.f, 100.f);
        ImGui::BeginChild("pipelineLeft", ImVec2(0, targetsH), true);
        ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kPipelineTargets));
        drawPipelineTargets();
        ImGui::EndChild();
        drawWorkspaceSplitter("##pipelineSourcesSplit", false, pipelineTargetsHeightFrac_, usableY);
        ImGui::BeginChild("pipelineSegmentList", ImVec2(0, 0), true);
        drawPipelineSegments();
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
        drawWorkspaceSplitter("##pipelineWorkspaceSplit", true, pipelineTargetsFrac_, usableX);
        ImGui::SameLine(0, 0);
    }
    ImGui::BeginChild("pipelineResults", ImVec2(0, 0), false);
    if (focus != 2) {
        const float usableY = std::max(1.f, ImGui::GetContentRegionAvail().y - split);
        const float graphH = focus == 1 ? 0.f
            : wa::pipeline_split::firstPaneSize(usableY, pipelineGraphHeightFrac_, 120.f, 220.f);
        ImGui::BeginChild("pipelineGraph", ImVec2(0, graphH), true);
        drawPipelineGraph();
        ImGui::EndChild();
        if (focus == 0)
            drawWorkspaceSplitter("##pipelineResultsSplit", false, pipelineGraphHeightFrac_, usableY);
    }
    if (focus != 1) {
        ImGui::BeginChild("pipelineCallLog", ImVec2(0, 0), true);
        drawPipelineCalls();
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::EndChild();
    drawLogRegion("pipelineLogRegion", "pipelineLog");
#ifdef WA_PERFORMANCE_FIXTURE
    PerformanceFixture::inspectorSubmitted(*this);
#endif
}

void AppUi::drawPipelinePaneHeader(const char* title, int pane) {
    ImGui::TextUnformatted(wa::ui_text::get(title));
    ImGui::SameLine();
    if (ImGui::SmallButton(wa::ui_text::id(pipelineFocusedPane_ == pane
            ? wa::ui_text::kPaneRestore : wa::ui_text::kPaneMaximize)))
        pipelineFocusedPane_ = pipelineFocusedPane_ == pane ? 0 : pane;
}

void AppUi::drawPipelineControls() {
    const auto startupStatus = pipelineAttaches_.startupStatus();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineRefresh)))
        refreshPipelineSessions();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kApplicationLoopbackPidLabel));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.0f);
    ImGui::InputText("##pipelineAttachPid", pipelineAttachPid_, sizeof(pipelineAttachPid_));
    ImGui::SameLine();
    uint32_t boxPid = 0;
    const bool boxPidOk = wa::pipeline_attach_ui::parseAttachPid(pipelineAttachPid_, boxPid);
    const auto attachedSet = pipelineAttachedPidSet();
    const auto batchStatus = pipelineAttaches_.batchStatus();
    const bool canAttach = wa::pipeline_attach_ui::canAttach(
        boxPidOk ? boxPid : 0u, attachedSet.find(boxPidOk ? boxPid : 0u) != attachedSet.end()) &&
        !pipelinePreparing_ && !batchStatus.running;
    if (!canAttach) ImGui::BeginDisabled();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineAttach)))
        runPipelineAttach();
    if (!canAttach) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canAttach);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineMethodsInitial))) {
        wa::ProcessInstance instance;
        const auto identity = wa::queryProcessInstance(boxPid, instance);
        if (identity) {
            pipelineInitialInstance_ = instance;
            const auto key = monitorInstanceKey(instance);
            if (pipelineInitialMethods_.find(key) == pipelineInitialMethods_.end())
                pipelineInitialMethods_[key].methods = pipelineAttaches_.lastMethods(instance);
            ImGui::OpenPopup("pipelineInitialMethods");
        } else pipelineAttachFailByPid_[boxPid] = identity.message;
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("pipelineInitialMethods")) {
        ImGui::Text("PID %u", pipelineInitialInstance_.pid);
        ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineMethodsReady));
        drawMonitorMethods(pipelineInitialMethods_[monitorInstanceKey(pipelineInitialInstance_)].methods, false);
        ImGui::EndPopup();
    }
    const bool canDetach = wa::pipeline_attach_ui::canDetachPid(boxPidOk ? boxPid : 0u, attachedSet,
        pipelineAttachFailByPid_) || (pipelinePreparing_ && pipelinePreparing_->pid == boxPid) ||
        (batchStatus.running && std::any_of(batchStatus.items.begin(), batchStatus.items.end(),
            [&](const auto& item) { return item.target.instance.pid == boxPid; }));
    continueWorkspaceRow(ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kPipelineDetach)).x + 20.f);
    if (!canDetach) ImGui::BeginDisabled();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineDetach)))
        runPipelineDetach();
    if (!canDetach) ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canDetachAll =
        wa::pipeline_attach_ui::canDetachAll(attachedSet, pipelineAttachFailByPid_) || pipelinePreparing_ || batchStatus.running || startupStatus.running;
    if (!canDetachAll) ImGui::BeginDisabled();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineDetachAll)))
        runPipelineDetachAll();
    if (!canDetachAll) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineStartup))) ImGui::OpenPopup("pipelineStartup");
    ImGui::SetNextWindowSize(ImVec2(650, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("pipelineStartup")) {
        ImGui::BeginDisabled(startupStatus.running);
        ImGui::InputText(wa::ui_text::get(wa::ui_text::kPipelineStartupExe), pipelineStartupExe_, sizeof(pipelineStartupExe_));
        ImGui::InputText(wa::ui_text::get(wa::ui_text::kPipelineStartupArguments), pipelineStartupArguments_, sizeof(pipelineStartupArguments_));
        ImGui::InputTextWithHint(wa::ui_text::get(wa::ui_text::kPipelineStartupDirectory),
            wa::ui_text::get(wa::ui_text::kPipelineStartupDirectoryHint), pipelineStartupDirectory_, sizeof(pipelineStartupDirectory_));
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineStartupMethods))) ImGui::OpenPopup("startupMethods");
        if (ImGui::BeginPopup("startupMethods")) {
            drawMonitorMethods(pipelineStartupMethods_, false);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!pipelineStartupExe_[0]);
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineStartupStart))) {
            wa::StartupRequest request;
            request.executable = utow(pipelineStartupExe_);
            request.arguments = utow(pipelineStartupArguments_);
            request.directory = utow(pipelineStartupDirectory_);
            const auto result = pipelineAttaches_.startStartup(request, pipelineStartupMethods_);
            pipelineStartupRequested_ = static_cast<bool>(result);
            pipelineStartupFailure_ = result ? "" : result.message;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!startupStatus.running);
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineStartupCancel))) pipelineAttaches_.cancelStartup();
        ImGui::EndDisabled();
        if (pipelineStartupRequested_) {
            const auto stage = startupStatus.stage;
            const char* status = !startupStatus.outcome.result ? wa::ui_text::kPipelineStartupFailed
                : startupStatus.outcome.committed ? wa::ui_text::kPipelineStartupCommitted
                : stage >= wa::StartupStage::ReceiverReady ? wa::ui_text::kPipelineStartupReleasing
                : stage >= wa::StartupStage::EntryHeld ? wa::ui_text::kPipelineStartupPreparing
                : wa::ui_text::kPipelineStartupWaiting;
            ImGui::TextWrapped("%s | PID %u", wa::ui_text::get(status), startupStatus.outcome.pid);
            if (!startupStatus.outcome.result) ImGui::TextWrapped("%s", startupStatus.outcome.result.message.c_str());
            if (!startupStatus.outcome.committed && startupStatus.outcome.targetExitConfirmed)
                ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineStartupExitConfirmed));
        }
        if (!pipelineStartupFailure_.empty()) ImGui::TextWrapped("%s", pipelineStartupFailure_.c_str());
        ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineStartupBoundary));
        ImGui::EndPopup();
    }
    std::vector<uint32_t> visibleOrder;
    for (const auto& group : pipelineTree_)
        for (const auto& process : group.processes) visibleOrder.push_back(process.pid);
    const auto selectedBatch = pipelineBatchSelection_.freeze(visibleOrder);
    char batchLabel[128]{};
    std::snprintf(batchLabel, sizeof(batchLabel), wa::ui_text::get(wa::ui_text::kPipelineAttachSelected), selectedBatch.size());
    continueWorkspaceRow(ImGui::CalcTextSize(batchLabel).x + 20.f);
    ImGui::BeginDisabled(selectedBatch.empty() || batchStatus.running || pipelinePreparing_.has_value());
    if (ImGui::Button(batchLabel)) {
        std::vector<wa::PipelineAttachController::Target> targets;
        for (const auto& request : selectedBatch) {
            const auto initial = pipelineInitialMethods_.find(monitorInstanceKey(request.instance));
            targets.push_back({request.instance, request.deviceId, request.flow,
                initial == pipelineInitialMethods_.end() ? UINT32_MAX : initial->second.methods});
        }
        const auto result = pipelineAttaches_.startBatch(targets);
        if (!result) logLines_.push_back("pipeline batch rejected: " + result.message);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!batchStatus.running);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineBatchCancel))) pipelineAttaches_.cancelBatch();
    ImGui::EndDisabled();
    if (!batchStatus.items.empty()) {
        using State = wa::PipelineAttachController::ItemState;
        unsigned success = 0, skipped = 0, failed = 0, cancelled = 0, unconfirmed = 0, pending = 0;
        for (const auto& item : batchStatus.items) {
            switch (item.state) {
            case State::Succeeded: ++success; break;
            case State::Skipped: ++skipped; break;
            case State::Cancelled: ++cancelled; break;
            case State::CleanupPending: ++unconfirmed; break;
            case State::Rejected: case State::Failed: case State::TargetChanged: ++failed; break;
            default: ++pending; break;
            }
        }
        ImGui::TextWrapped(wa::ui_text::get(wa::ui_text::kPipelineBatchSummary), success, skipped, failed, cancelled, unconfirmed, pending);
        if (ImGui::TreeNodeEx(wa::ui_text::id(wa::ui_text::kPipelineBatchResults), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginChild("pipelineBatchResults", ImVec2(0, 76), true);
            for (const auto& item : batchStatus.items) {
                const char* text = wa::ui_text::kPipelineBatchQueued;
                switch (item.state) {
                case State::Preparing: text = wa::ui_text::kPipelineBatchPreparing; break;
                case State::Injecting: text = wa::ui_text::kPipelineBatchInjecting; break;
                case State::Installing: text = wa::ui_text::kPipelineBatchInstalling; break;
                case State::Succeeded: text = wa::ui_text::kPipelineBatchSucceeded; break;
                case State::Skipped: text = wa::ui_text::kPipelineBatchSkipped; break;
                case State::Rejected: text = wa::ui_text::kPipelineBatchRejected; break;
                case State::Failed: text = wa::ui_text::kPipelineBatchFailed; break;
                case State::TargetChanged: text = wa::ui_text::kPipelineBatchTargetChanged; break;
                case State::Cancelled: text = wa::ui_text::kPipelineBatchCancelled; break;
                case State::CleanupPending: text = wa::ui_text::kPipelineBatchCleanup; break;
                default: break;
                }
                ImGui::Text("PID %u: %s", item.target.instance.pid, wa::ui_text::get(text));
                if (!item.result) ImGui::SetItemTooltip("%s", item.result.message.c_str());
            }
            ImGui::EndChild();
            ImGui::TreePop();
        }
    }
    if (pipelinePreparing_)
        ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelineAttachPreparing), pipelinePreparing_->pid);
    const auto receiver = pipelineAttaches_.snapshot();
    if (receiver.cleanupPending)
        ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelineAttachCleanup), receiver.cleanupPending);
    if (receiver.droppedBatches || receiver.rejectedCommands)
        ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelineAttachOverflow),
            static_cast<unsigned long long>(receiver.droppedCalls),
            static_cast<unsigned long long>(receiver.droppedBatches),
            static_cast<unsigned long long>(receiver.rejectedCommands));
    if (pipelineExport_.busy()) {
        ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineExportBusy));
        ImGui::SameLine();
        if (ImGui::Button(wa::ui_text::get(wa::ui_text::kPipelineCancel))) pipelineExport_.cancel();
    }
    {
        uint32_t statusPid = 0;
        wa::pipeline_attach_ui::parseAttachPid(pipelineAttachPid_, statusPid);
        const wa::PipelineAttachUiState boxSt =
            wa::pipeline_attach_ui::statusForPid(statusPid, attachedSet, pipelineAttachFailByPid_);
        if (boxSt == wa::PipelineAttachUiState::Attached) {
            ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineAttached));
        } else if (boxSt == wa::PipelineAttachUiState::Failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.55f, 0.55f, 1.f));
            ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineAttachFailed));
            const auto fit = pipelineAttachFailByPid_.find(statusPid);
            if (fit != pipelineAttachFailByPid_.end())
                ImGui::TextWrapped("%s", fit->second.c_str());
            ImGui::PopStyleColor();
        } else if (statusPid != 0) {
            ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineAttachDetached));
        }
        const auto attachedList = pipelineAttaches_.attachedPids();
        if (!attachedList.empty()) {
            ImGui::Text("%s: %s", wa::ui_text::get(wa::ui_text::kPipelineAttachedPids),
                        wa::pipeline_attach_ui::formatAttachedPids(attachedList).c_str());
        }
    }
}

void AppUi::drawPipelineTargets() {
    ImGui::SetNextItemWidth(220.f);
    ImGui::InputTextWithHint("##processSearch", wa::ui_text::get(wa::ui_text::kPipelineProcessSearch),
                             pipelineProcessFilter_, sizeof(pipelineProcessFilter_));
    continueWorkspaceRow(ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kPipelineShowSelf)).x + 32.f);
    if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kPipelineShowSelf), &pipelineShowSelf_))
        refreshPipelineSessions();
    continueWorkspaceRow(ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kPipelineAudioOnly)).x + 32.f);
    if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kPipelineAudioOnly), &pipelineAudioOnly_)) {
        if (!pipelineAudioOnly_) refreshPipelineProcesses();
        rebuildPipelineProcessTree();
    }
    const auto attachedSet = pipelineAttachedPidSet();
    // Process list has no change notification; re-snapshot it every 2 s in "all" mode.
    if (!pipelineAudioOnly_ && GetTickCount64() - pipelineProcessesTick_ >= 2000ull)
        refreshPipelineProcesses();

    // Selection changes only update the PID box; multi-attach keeps other hooks.
    const auto switchPipelinePid = [this](uint32_t /*pid*/) {
        pipelineProbes_.clear();
    };
    const auto hookText = [](wa::PipelineAttachUiState st) {
        if (st == wa::PipelineAttachUiState::Attached)
            return wa::ui_text::get(wa::ui_text::kPipelineAttached);
        if (st == wa::PipelineAttachUiState::Failed)
            return wa::ui_text::get(wa::ui_text::kPipelineAttachFailed);
        return "";
    };
    const auto drawBatchCheckbox = [&](uint32_t pid, uint64_t created, int session) {
        bool checked = pipelineBatchSelection_.contains(pid, created);
        ImGui::PushID(static_cast<int>(pid));
        ImGui::PushID(session);
        if (ImGui::Checkbox("##batch", &checked)) {
            wa::pipeline_attach_ui::AttachRequest request;
            request.pid = pid;
            request.instance = {pid, created};
            if (session >= 0) {
                const auto& row = pipelineSessions_[static_cast<size_t>(session)];
                request.deviceId = row.deviceId;
                request.flow = row.flow;
                request.sessionKey = wa::liveSessionAttachKey(row);
            }
            wa::Result identity = wa::Result::Ok();
            if (checked && !request.instance) identity = wa::queryProcessInstance(pid, request.instance);
            if (identity) pipelineBatchSelection_.set(std::move(request), checked);
            else pipelineAttachFailByPid_[pid] = identity.message;
        }
        ImGui::PopID();
        ImGui::PopID();
        ImGui::SameLine();
    };

    if (pipelineTree_.empty()) {
        ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineEmpty));
    } else if (ImGui::BeginTable("pipelineProcesses", 7,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                     ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate,
                                 ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineColProcess),
                                ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort,
                                2.f);
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kApplicationLoopbackPidLabel),
                                ImGuiTableColumnFlags_WidthFixed, 64.f);
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineColDevice),
                                ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_DefaultHide,
                                1.5f);
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineColVol),
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_DefaultHide,
                                48.f);
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineColMute),
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_DefaultHide,
                                48.f);
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineColState),
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 72.f);
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineColAttach),
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                                52.f);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsDirty) {
                if (sortSpecs->SpecsCount > 0) {
                    pipelineProcessSortCol_ = sortSpecs->Specs[0].ColumnIndex;
                    pipelineProcessSortAsc_ =
                        sortSpecs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
                } else {
                    // Tristate cleared: restore default name ascending.
                    pipelineProcessSortCol_ = 0;
                    pipelineProcessSortAsc_ = true;
                }
                applyPipelineProcessSort();
                sortSpecs->SpecsDirty = false;
            }
        }
        // Name group / process with audio: muted blue. Active session row: green.
        const ImU32 audioProcessBg = IM_COL32(48, 72, 112, 120);
        const ImU32 activeSessionBg = IM_COL32(40, 120, 64, 150);

        const auto drawProcessRow = [&](const wa::ProcessTreeRow& p, int treeIdSalt) {
            const bool hasAudio = !p.sessions.empty();
            ImGui::TableNextRow();
            if (hasAudio) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, audioProcessBg);
            ImGui::TableSetColumnIndex(0);
            drawBatchCheckbox(p.pid, p.sessions.empty() ? 0 : pipelineSessions_[static_cast<size_t>(p.sessions.front())].processCreated,
                              p.sessions.empty() ? -1 : p.sessions.front());
            const bool procSelected = pipelineSelectedPid_ == p.pid;
            ImGuiTreeNodeFlags tf = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
            if (!hasAudio) tf |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (procSelected) tf |= ImGuiTreeNodeFlags_Selected;
            if (hasAudio) tf |= ImGuiTreeNodeFlags_DefaultOpen;
            // Audio sessions are visible by default; explicit folding is remembered per PID.
            const intptr_t nodeId =
                (static_cast<intptr_t>(treeIdSalt) << 32) ^ static_cast<intptr_t>(p.pid);
            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(nodeId), tf, "%s",
                                                p.name.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                if (!procSelected) switchPipelinePid(p.pid);
                if (!procSelected || pipelineSelected_ < 0)
                    pipelineSelected_ = wa::pipeline_attach_ui::uniqueSessionForPid(pipelineSessions_, p.pid);
                pipelineSelectedPid_ = p.pid;
                std::snprintf(pipelineAttachPid_, sizeof(pipelineAttachPid_), "%u", p.pid);
                rebuildPipelineGraph();
            }
            ImGui::SetItemTooltip("%s (PID %u)", p.name.c_str(), p.pid);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", p.pid);
            ImGui::TableSetColumnIndex(2);
            if (hasAudio)
                ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelineSessionCountFmt),
                            static_cast<int>(p.sessions.size()));
            ImGui::TableSetColumnIndex(5);
            if (p.hasActiveSession) ImGui::TextUnformatted("Active");
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(hookText(wa::pipeline_attach_ui::statusForPid(
                p.pid, attachedSet, pipelineAttachFailByPid_)));
            if (!open || !hasAudio) return;
            for (const int i : p.sessions) {
                if (i < 0 || i >= (int)pipelineSessions_.size()) continue;
                const auto& srow = pipelineSessions_[(size_t)i];
                const std::string tip = wa::ui_text::formatLiveSessionTooltip(srow);
                ImGui::TableNextRow();
                if (srow.state == "Active")
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, activeSessionBg);
                ImGui::TableSetColumnIndex(0);
                drawBatchCheckbox(srow.processId, srow.processCreated, i);
                const bool selected = (i == pipelineSelected_);
                char label[512];
                std::snprintf(label, sizeof(label), "%s: %s###ps%d",
                              srow.flow == wa::PipelineFlow::Capture
                                  ? wa::ui_text::get(wa::ui_text::kPipelineFlowCapture)
                                  : wa::ui_text::get(wa::ui_text::kPipelineFlowRender),
                              srow.deviceName.c_str(), i);
                if (ImGui::Selectable(label, selected)) {
                    if (pipelineSelected_ != i) switchPipelinePid(srow.processId);
                    pipelineSelected_ = i;
                    pipelineSelectedPid_ = srow.processId;
                    wa::pipeline_attach_ui::copyLiveSessionPidToBuffer(
                        pipelineSessions_, i, pipelineAttachPid_, sizeof(pipelineAttachPid_));
                    rebuildPipelineGraph();
                }
                ImGui::SetItemTooltip("%s", tip.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", srow.processId);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(srow.deviceName.c_str());
                ImGui::SetItemTooltip("%s", tip.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f", static_cast<double>(srow.sessionVolume));
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(srow.sessionMute
                                           ? wa::ui_text::get(wa::ui_text::kPipelineMuteYes)
                                           : wa::ui_text::get(wa::ui_text::kPipelineMuteNo));
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(srow.state.c_str());
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(hookText(pipelineAttachUiFor(srow)));
            }
            ImGui::TreePop();
        };

        size_t matchingProcesses = 0;
        const auto matchesProcess = [&](const wa::ProcessTreeRow& process) {
            return wa::call_log_filter::fuzzyMatch(process.name + " " + std::to_string(process.pid),
                                                   pipelineProcessFilter_);
        };
        for (int gi = 0; gi < (int)pipelineTree_.size(); ++gi) {
            const wa::ProcessNameGroup& g = pipelineTree_[(size_t)gi];
            const auto matches = static_cast<size_t>(std::count_if(g.processes.begin(), g.processes.end(), matchesProcess));
            matchingProcesses += matches;
            if (!matches) continue;
            if (g.processes.size() == 1) {
                // Unique name: single process row; sessions nest under the PID.
                drawProcessRow(g.processes[0], gi + 1);
                continue;
            }
            // Same executable name: parent name node, then each PID (+ sessions).
            ImGui::TableNextRow();
            if (g.hasAudio) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, audioProcessBg);
            ImGui::TableSetColumnIndex(0);
            // Expand audio groups initially; ImGui remembers later manual folding.
            const ImGuiTreeNodeFlags ntf = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow
                | (g.hasAudio ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
            if (pipelineProcessFilter_[0]) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            const bool nameOpen = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<intptr_t>(-(gi + 1))), ntf, "%s",
                g.name.c_str());
            if (ImGui::IsItemClicked()) {
                // Name-group root shows the primary instance PID; fill it for Attach.
                if (g.rootPid != 0) {
                    if (pipelineSelectedPid_ != g.rootPid) switchPipelinePid(g.rootPid);
                    pipelineSelected_ = wa::pipeline_attach_ui::uniqueSessionForPid(pipelineSessions_, g.rootPid);
                    pipelineSelectedPid_ = g.rootPid;
                    std::snprintf(pipelineAttachPid_, sizeof(pipelineAttachPid_), "%u",
                                  g.rootPid);
                    rebuildPipelineGraph();
                }
            }
            ImGui::SetItemTooltip("%s (root PID %u)", g.name.c_str(), g.rootPid);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", g.rootPid);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelineProcessCountFmt),
                        static_cast<int>(g.processes.size()));
            ImGui::TableSetColumnIndex(5);
            if (g.hasActiveSession) ImGui::TextUnformatted("Active");
            if (!nameOpen) continue;
            for (const wa::ProcessTreeRow& proc : g.processes)
                if (matchesProcess(proc)) drawProcessRow(proc, gi + 1);
            ImGui::TreePop();
        }
        if (!matchingProcesses) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineNoProcessMatch));
        }
        ImGui::EndTable();
    }
}

void AppUi::drawPipelineSegments() {
    const auto segmentStatus = [](wa::CallLogStore::SegmentState state) {
        return wa::ui_text::get(state == wa::CallLogStore::SegmentState::Monitoring
            ? wa::ui_text::kPipelineMonitoring : state == wa::CallLogStore::SegmentState::Exited
            ? wa::ui_text::kPipelineExited : wa::ui_text::kPipelineDetachedHistory);
    };
    const auto drawSegments = [&](bool active) {
        for (const auto& segment : pipelineCallStore_.segments(active)) {
            ImGui::PushID(static_cast<int>(segment.id));
            std::string label = segment.name + "\nPID " + std::to_string(segment.instance.pid) +
                " | #" + std::to_string(segment.id) + " | " + segmentStatus(segment.state) +
                "\n" + wa::pipeline_attach_ui::segmentTime(segment.startedMs);
            if (ImGui::Selectable(label.c_str(), pipelineSegmentSelection_.selected() == segment.id,
                                  ImGuiSelectableFlags_None, ImVec2(0, ImGui::GetTextLineHeight() * 3.f))) {
                pipelineSegmentSelection_.select(pipelineCallStore_, segment.id);
                pipelineCallSelected_ = {};
                pipelinePumpSelectionExpired_ = false;
            }
            ImGui::SetItemTooltip("%s", label.c_str());
            ImGui::PopID();
        }
    };
    ImGui::SeparatorText(wa::ui_text::get(wa::ui_text::kPipelineMonitoring));
    drawSegments(true);
    if (const auto* current = pipelineCallStore_.segment(pipelineSegmentSelection_.selected());
        current && current->state != wa::CallLogStore::SegmentState::Monitoring)
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader(wa::ui_text::get(wa::ui_text::kPipelineHistory)))
        drawSegments(false);
}

void AppUi::drawPipelineCalls() {
    drawPipelinePaneHeader(wa::ui_text::kPipelineCallLog, 2);
    const auto segmentStatus = [](wa::CallLogStore::SegmentState state) {
        return wa::ui_text::get(state == wa::CallLogStore::SegmentState::Monitoring
            ? wa::ui_text::kPipelineMonitoring : state == wa::CallLogStore::SegmentState::Exited
            ? wa::ui_text::kPipelineExited : wa::ui_text::kPipelineDetachedHistory);
    };
    const auto* segment = pipelineCallStore_.segment(pipelineSegmentSelection_.selected());
    if (segment) {
        ImGui::TextWrapped(wa::ui_text::get(wa::ui_text::kPipelineCallSourceFmt), segment->name.c_str(),
            segment->instance.pid, static_cast<unsigned long long>(segment->id), segmentStatus(segment->state));
        if (pipelineSelectedPid_ && pipelineSelectedPid_ != segment->instance.pid)
            ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kSeparateTargets));
        ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kPipelineMethodsShowWindow), &pipelinePump_);
        ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kPipelineRecentHint));
        continueWorkspaceRow(ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kPipelineSaveCalls)).x + 20.f);
        const bool canSave = pipelineCallStore_.segment(pipelineSegmentSelection_.selected()) && !pipelineExport_.busy();
        if (!canSave) ImGui::BeginDisabled();
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineSaveCalls)))
            savePipelineCallLog();
        if (!canSave) ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kPipelineExportHint));
        ImGui::SameLine();
        ImGui::BeginDisabled(segment->state != wa::CallLogStore::SegmentState::Monitoring);
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineDetachCurrent)))
            runPipelineDetach(segment->instance.pid);
        ImGui::EndDisabled();
        if (ImGui::GetWindowWidth() > 600.f) ImGui::SameLine();
        auto& clearState = pipelineClearStates_[segment->id];
        ImGui::BeginDisabled(clearState.pending);
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineClearCurrent))) {
            if (segment->state == wa::CallLogStore::SegmentState::Monitoring) {
                const auto result = pipelineAttaches_.clear(segment->instance, segment->token);
                clearState.pending = static_cast<bool>(result);
                clearState.failure = result ? "" : result.message;
            } else {
                const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (pipelineCallStore_.clearSegment(segment->id, segment->observationGeneration, timestamp)) {
                    pipelineCallSelected_ = {};
                    pipelinePumpSelectionExpired_ = false;
                    clearState.failure.clear();
                    applyPipelineJoin();
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kPipelineClearHint));
        continueWorkspaceRow(ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kPipelineStatisticsTab)).x + 20.f);
        ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kPipelineStatisticsTab), &pipelineShowStatistics_);
        continueWorkspaceRow(ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kPipelineMethods)).x + 32.f);
        ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kPipelineMethods), &pipelineShowMethods_);
        if (clearState.pending) ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineClearPending));
        if (!clearState.failure.empty()) {
            ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineClearFailed));
            ImGui::TextWrapped("%s", clearState.failure.c_str());
        }
    }
    if (pipelineExport_.busy()) {
        ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineExportBusy));
        ImGui::SameLine();
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineCancel))) pipelineExport_.cancel();
    }
    if (!segment) {
        ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineSegmentHint));
        return;
    }
    ImGui::TextDisabled(wa::ui_text::get(wa::ui_text::kPipelineReceivedFmt),
        static_cast<unsigned long long>(segment->count), static_cast<unsigned long long>(segment->windowRetained));
    const auto& snapshot = segment->transport.statistics;
    if (snapshot.known) {
        uint64_t completed = 0, failed = 0, discontinuities = 0;
        bool unknown = false;
        for (size_t i = 0; i < snapshot.methods.size(); ++i) {
            if (wa::kMonitorMethods[i].control) continue;
            const auto& count = snapshot.methods[i];
            completed += count.completed;
            failed += count.failed;
            discontinuities += count.discontinuities;
            unknown = unknown || count.unknown;
        }
        ImGui::TextWrapped(wa::ui_text::get(wa::ui_text::kPipelineCountSummaryFmt),
            static_cast<unsigned long long>(completed), static_cast<unsigned long long>(failed),
            static_cast<unsigned long long>(discontinuities), wa::ui_text::get(unknown
                ? wa::ui_text::kPipelineTransportOtherUnknown : wa::ui_text::kPipelineStatisticsExact));
    } else ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kPipelineStatisticsUnknown));
    ImGui::SetItemTooltip("%s", wa::ui_text::pipelineTransportText(segment->transport).c_str());
    const float usableX = std::max(1.f, ImGui::GetContentRegionAvail().x - wa::pipeline_split::kSplitterPx);
    const bool showSidebar = pipelineShowMethods_ || pipelineShowStatistics_;
    const float methodsW = showSidebar
        ? wa::pipeline_split::firstPaneSize(usableX, pipelineMethodsFrac_, 160.f, 240.f) : 0.f;
    ImGui::BeginChild("pipelineCallResults", ImVec2(showSidebar ? usableX - methodsW : 0.f, 0), false);
    frameProbe_.addWork("control-total", pipelineCallStore_.size());
    frameProbe_.addWork("control-capacity", pipelineCallStore_.entries().capacity());
    frameProbe_.addWork("recording-copy-total", 0);
    frameProbe_.addWork("presentation-copy-total", 0);
    frameProbe_.addWork("pump-total", pipelineCallStore_.pumpEntries().size());

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineCallFilter));
    ImGui::SameLine();
    const float filterRowAvail = ImGui::GetContentRegionAvail().x;
    const bool wideFilters = filterRowAvail >= 500.f;
    const float ifaceComboW = std::min(160.f, filterRowAvail * 0.25f);
    const float filterLabelsW = ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kPipelineCallColIface)).x
        + ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kPipelineCallFailuresOnly)).x
        + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 4.f;
    const float textFilterW = wideFilters ? std::max(80.f, filterRowAvail - ifaceComboW - filterLabelsW) : filterRowAvail;
    ImGui::SetNextItemWidth(textFilterW);
    bool filterEdited = ImGui::InputTextWithHint(
        "##pipelineCallFilter", wa::ui_text::get(wa::ui_text::kPipelineCallFilterHint),
        pipelineCallFilter_, sizeof(pipelineCallFilter_));
#ifdef WA_PERFORMANCE_FIXTURE
    filterEdited = PerformanceFixture::filterEdited() || filterEdited;
#endif

    if (wideFilters) ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineCallColIface));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ifaceComboW);
    const auto ifaceChoices = pipelineCallStore_.ifaces(pipelineSegmentSelection_.selected());
    const char* ifacePreview = pipelineCallIfaceFilter_[0]
                                   ? pipelineCallIfaceFilter_
                                   : wa::ui_text::get(wa::ui_text::kPipelineCallIfaceAll);
    if (ImGui::BeginCombo("##pipelineCallIface", ifacePreview)) {
        const bool allSelected = pipelineCallIfaceFilter_[0] == '\0';
        if (ImGui::Selectable(wa::ui_text::get(wa::ui_text::kPipelineCallIfaceAll), allSelected)) {
            pipelineCallIfaceFilter_[0] = '\0';
            filterEdited = true;
        }
        for (const auto& iface : ifaceChoices) {
            const bool sel = wa::call_log_filter::asciiEqualsIgnoreCase(iface, pipelineCallIfaceFilter_);
            if (ImGui::Selectable(iface.c_str(), sel)) {
                std::snprintf(pipelineCallIfaceFilter_, sizeof(pipelineCallIfaceFilter_), "%s",
                              iface.c_str());
                filterEdited = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Checkbox(wa::ui_text::get(wa::ui_text::kPipelineCallFailuresOnly),
                        &pipelineCallFailuresOnly_)) {
        filterEdited = true;
    }

    wa::call_log_filter::FilterOpts filterOpts;
    filterOpts.text = pipelineCallFilter_;
    filterOpts.iface = pipelineCallIfaceFilter_;
    filterOpts.failuresOnly = pipelineCallFailuresOnly_;
    filterOpts.attach = pipelineSegmentSelection_.selected();
    const auto visible = pipelineCallStore_.view(filterOpts, {&frameProbe_,
        [](void* context, wa::CallLogStage stage, bool begin, size_t) {
            auto& probe = *static_cast<wa::FrameProbe*>(context);
            const auto slice = stage == wa::CallLogStage::Sort ? wa::FrameSlice::CallSort
                : wa::FrameSlice::CallMatch;
            if (begin) probe.begin(slice); else probe.end(slice);
        }});
    frameProbe_.addWork("call-matching-total", visible.size());
    frameProbe_.addWork("call-query-bytes", visible.indexBytes());
    frameProbe_.addWork("control-sort-records", visible.work().sorted);
    frameProbe_.addWork("control-match-records", visible.work().matched);
    frameProbe_.addWork("pump-match-records", visible.work().pumpMatched);
    frameProbe_.addWork("query-invalidated", visible.work().queryChanged);
    frameProbe_.addWork("control-invalidated", visible.work().controlChanged);
    frameProbe_.addWork("pump-invalidated", visible.work().pumpChanged);
    const bool anyFilterActive = filterOpts.text.size() > 0 || filterOpts.iface.size() > 0
                                 || filterOpts.failuresOnly;
    if (filterEdited) {
        if (visible.size() == 0) {
            // Keep prior selection so Parameters still show the last pick.
        } else {
            if (visible.position(pipelineCallSelected_) == wa::CallLogStore::kNotFound) {
                pipelineCallSelected_ = visible.at(0)->id;
                pipelinePumpSelectionExpired_ = false;
                pipelineCallScrollToSelected_ = true;
            }
        }
    }

    if (pipelineCallStore_.size() == 0 && pipelineCallStore_.pumpEntries().empty()) {
        ImGui::TextWrapped("%s", wa::ui_text::pipelineCallLogEmptyText(pipelineAttaches_.anyAttached(),
                                                                      pipelinePump_));
        if (pipelinePumpSelectionExpired_)
            ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelinePumpExpired));
    } else {
        const float callAvailX = ImGui::GetContentRegionAvail().x;
        const float callHSplit = wa::pipeline_split::kSplitterPx;
        // Call list : Parameters ~= 4:6 default; draggable.
        const float callUsableX = (std::max)(1.f, callAvailX - callHSplit);
        const float tableW = wa::pipeline_split::firstPaneSize(
            callUsableX, pipelineCallTableFrac_, 160.f, 200.f);
        const float paramsW = callUsableX - tableW;

        ImGui::BeginChild("pipelineCallTable", ImVec2(tableW, 0), false);
        if (visible.size() == 0 && anyFilterActive) {
            ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineCallFilterHint));
        } else if (ImGui::BeginTable("pipelineCalls", 5,
                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                                 ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable)) {
            wa::FrameBracket rows(frameProbe_, wa::FrameSlice::CallRows);
#ifdef WA_PERFORMANCE_FIXTURE
            PerformanceFixture::scrollCalls();
#endif
            ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColPid),
                                    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 64.f);
            ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColCall),
                                    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 1.f);
            ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColArgs),
                                    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.f);
            ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColHr),
                                    ImGuiTableColumnFlags_WidthFixed, 88.f);
            ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColStream),
                                    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 64.f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            uint64_t visibleRows = 0;
            uint64_t submittedRows = 0;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            if (pipelineCallScrollToSelected_) {
                const auto position = visible.position(pipelineCallSelected_);
                if (position != wa::CallLogStore::kNotFound)
                    clipper.IncludeItemByIndex(static_cast<int>(position));
            }
            while (clipper.Step()) {
                for (int vi = clipper.DisplayStart; vi < clipper.DisplayEnd; ++vi) {
                    const auto& record = *visible.at(static_cast<size_t>(vi));
                    const auto& call = record.call;
                    ++submittedRows;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%u", call.pid);
                    ImGui::TableSetColumnIndex(1);
                    const bool selected = pipelineCallSelected_ == record.id;
                    const auto callName = wa::call_log_filter::singleLine(wa::call_log_filter::formatCallName(call));
                    char callLabel[320];
                    std::snprintf(callLabel, sizeof(callLabel), "%s###pcall%llu-%llu-%u-%llu", callName.c_str(),
                        static_cast<unsigned long long>(record.id.round),
                        static_cast<unsigned long long>(record.id.attach), static_cast<unsigned>(record.id.lane),
                        static_cast<unsigned long long>(record.id.sequence));
                    if (ImGui::Selectable(callLabel, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        pipelineCallSelected_ = record.id;
                        pipelinePumpSelectionExpired_ = false;
                    }
                    ImGui::SetItemTooltip("%s\n%s", callName.c_str(), call.args.c_str());
                    if (ImGui::IsItemVisible()) ++visibleRows;
                    if (selected && pipelineCallScrollToSelected_) {
                        ImGui::SetScrollHereY(0.25f);
                        pipelineCallScrollToSelected_ = false;
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(wa::call_log_filter::singleLine(call.args).c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", call.args.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(wa::CallLogStore::formatHresult(call.hresult).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%u", call.streamId);
                }
            }
            frameProbe_.addWork("call-row-submissions", submittedRows);
            frameProbe_.addWork("call-row-visible", visibleRows);
            ImGui::EndTable();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);
        {
            const float splitH = ImGui::GetContentRegionAvail().y;
            ImGui::InvisibleButton("##pipelineCallParamsSplit",
                                   ImVec2(callHSplit, (std::max)(1.0f, splitH)));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive()) {
                pipelineCallTableFrac_ = wa::pipeline_split::nudgeFrac(
                    pipelineCallTableFrac_, ImGui::GetIO().MouseDelta.x, callUsableX, 0.20f,
                    0.80f);
            }
            const ImVec2 smn = ImGui::GetItemRectMin(), smx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2((smn.x + smx.x) * 0.5f, smn.y), ImVec2((smn.x + smx.x) * 0.5f, smx.y),
                ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        }
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("pipelineCallParams", ImVec2(paramsW, 0), true);
        ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kPipelineCallParams));
        if (const auto* selected = pipelineCallStore_.find(pipelineCallSelected_)) {
            ImGui::TextWrapped("%s", wa::call_log_filter::formatCallName(selected->call).c_str());
            ImGui::TextDisabled("PID %u | %s", selected->call.pid,
                                wa::CallLogStore::formatHresult(selected->call.hresult).c_str());
            if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineCopyCall)))
                ImGui::SetClipboardText(wa::CallLogStore::toJsonLine(*selected).c_str());
        }
        // Dedicated scroll child: table ScrollY without outer_size was clipping rows.
        ImGui::BeginChild("pipelineCallParamsBody", ImVec2(0, 0), false,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
#ifdef WA_PERFORMANCE_FIXTURE
        PerformanceFixture::scrollParameters();
#endif
        const auto* selectedRecord = pipelineCallStore_.find(pipelineCallSelected_);
        if (!selectedRecord) {
            ImGui::TextWrapped("%s", wa::ui_text::get(pipelinePumpSelectionExpired_
                ? wa::ui_text::kPipelinePumpExpired : wa::ui_text::kPipelineCallParamsSelectHint));
        } else {
            const auto& sel = selectedRecord->call;
            if (sel.parameters.empty()) {
                ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineCallParamsEmpty));
            } else if (ImGui::BeginTable("callParams", 4,
                                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_SizingStretchProp |
                                            ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColType), ImGuiTableColumnFlags_WidthStretch, 1.f);
                ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColName), ImGuiTableColumnFlags_WidthStretch, 1.3f);
                ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColPre), ImGuiTableColumnFlags_WidthStretch, 2.f);
                ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kPipelineCallColPost), ImGuiTableColumnFlags_WidthStretch, 2.f);
                ImGui::TableHeadersRow();
                for (const auto& a : sel.parameters) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextWrapped("%s", a.type.c_str());
                    ImGui::TableSetColumnIndex(1);
                    if (a.depth > 0) {
                        ImGui::TextWrapped("%*s%s", static_cast<int>(a.depth) * 2, "", a.name.c_str());
                    } else {
                        ImGui::TextWrapped("%s", a.name.c_str());
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped("%s", a.pre.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextWrapped("%s", a.post.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }
    ImGui::EndChild();
    if (showSidebar) {
        ImGui::SameLine(0, 0);
        drawWorkspaceSplitter("##pipelineMethodsSplit", true, pipelineMethodsFrac_, usableX, -1.f);
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("pipelineSidebar", ImVec2(0, 0), false);
        const float sidebarHeight = std::max(1.f, ImGui::GetContentRegionAvail().y - wa::pipeline_split::kSplitterPx);
        const float statsHeight = pipelineShowMethods_
            ? wa::pipeline_split::firstPaneSize(sidebarHeight, pipelineStatisticsFrac_, 100.f, 150.f) : 0.f;
        if (pipelineShowStatistics_) {
            ImGui::BeginChild("pipelineStatistics", ImVec2(0, statsHeight), true);
            ImGui::SeparatorText(wa::ui_text::get(wa::ui_text::kPipelineStatisticsTab));
            drawPipelineStatistics(*segment);
            ImGui::EndChild();
        }
        if (pipelineShowStatistics_ && pipelineShowMethods_)
            drawWorkspaceSplitter("##pipelineStatisticsSplit", false, pipelineStatisticsFrac_, sidebarHeight);
        if (pipelineShowMethods_) {
            ImGui::BeginChild("pipelineMethods", ImVec2(0, 0), true);
            ImGui::SeparatorText(wa::ui_text::get(wa::ui_text::kPipelineMethods));
            drawPipelineMethods(*segment);
            ImGui::EndChild();
        }
        ImGui::EndChild();
    }
}

void AppUi::drawPipelineMethods(const wa::CallLogStore::Segment& value) {
    const auto* segment = &value;
    const bool readOnly = segment->state != wa::CallLogStore::SegmentState::Monitoring;
    auto found = pipelineMethodDrafts_.find(segment->id);
    if (found == pipelineMethodDrafts_.end())
        found = pipelineMethodDrafts_.emplace(segment->id,
            wa::pipeline_attach_ui::MethodDraft{segment->methods}).first;
    auto& draft = found->second;
    if (readOnly) draft.methods = segment->methods;
    ImGui::TextUnformatted(wa::ui_text::get(draft.pending ? wa::ui_text::kPipelineMethodsPending
        : draft.dirty(segment->methods) ? wa::ui_text::kPipelineMethodsDraft : wa::ui_text::kPipelineMethodsApplied));
    if (!draft.failure.empty()) {
        ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kPipelineMethodsFailed));
        ImGui::TextWrapped("%s", draft.failure.c_str());
    }
    ImGui::BeginChild("methodChoices", ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.f)), true);
    drawMonitorMethods(draft.methods, readOnly || draft.pending);
    ImGui::EndChild();
    ImGui::BeginDisabled(readOnly || draft.pending || !draft.dirty(segment->methods));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kPipelineMethodsApply))) {
        const auto result = pipelineAttaches_.configure(segment->instance, segment->token, draft.methods);
        draft.pending = static_cast<bool>(result);
        draft.failure = result ? "" : result.message;
    }
    ImGui::EndDisabled();
}

void AppUi::drawPipelineStatistics(const wa::CallLogStore::Segment& value) {
    const auto* segment = &value;
    ImGui::BeginChild("segmentStatistics", ImVec2(0, 0), false);
    if (segment->startup) {
        const auto& origin = *segment->startup;
        ImGui::TextWrapped(wa::ui_text::get(wa::ui_text::kPipelineStartupSourceFmt), origin.mainThreadId,
            static_cast<unsigned long long>(origin.entryAddress), origin.protocolLayout,
            origin.acceptedAttempts, origin.rejectedAttempts);
        ImGui::SetItemTooltip("%s\n%s", origin.hookPath.c_str(),
            wa::ui_text::get(wa::ui_text::kPipelineStartupBoundary));
        if (!segment->count && !segment->windowReceived)
            ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineStartupEmpty));
    }
    ImGui::TextDisabled(wa::ui_text::get(wa::ui_text::kPipelineStatisticsStartFmt),
        wa::pipeline_attach_ui::segmentTime(segment->statisticsStartedMs).c_str(), segment->observationGeneration);
    if (segment->state != wa::CallLogStore::SegmentState::Monitoring)
        ImGui::TextWrapped(wa::ui_text::get(wa::ui_text::kPipelineHistorySnapshotFmt),
            wa::pipeline_attach_ui::segmentTime(segment->snapshotMs).c_str());
    ImGui::TextWrapped("%s", wa::ui_text::pipelineTransportText(segment->transport).c_str());
    ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kPipelineTransportHint));
    if (segment->transport.snapshotMs > 0)
        ImGui::TextDisabled(wa::ui_text::get(wa::ui_text::kPipelineTransportSnapshotFmt),
            wa::pipeline_attach_ui::segmentTime(segment->transport.snapshotMs).c_str());
    ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelineWindowCountsFmt),
        static_cast<unsigned long long>(segment->windowRetained),
        static_cast<unsigned long long>(segment->windowReceived),
        static_cast<unsigned long long>(segment->windowEvicted));
    if (ImGui::CollapsingHeader(wa::ui_text::id(wa::ui_text::kPipelineSourceStatistics), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", wa::ui_text::pipelineStatisticsText(segment->transport.statistics).c_str());
        ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineStatisticsHint));
    }
    ImGui::EndChild();
}

void AppUi::drawPipelineGraph() {
    drawPipelinePaneHeader(wa::ui_text::kPipelineGraph, 1);
    ImGui::SameLine();
    const bool canProbe = pipelineSelected_ >= 0 && !pipelineProbing_;
    ImGui::BeginDisabled(!canProbe);
    if (ImGui::SmallButton(wa::ui_text::id(wa::ui_text::kPipelineProbe))) runPipelineProbe();
    ImGui::EndDisabled();
    const wa::EtwWatchStatus etwStatus = pipelineEtw_.status();
    if (etwStatus == wa::EtwWatchStatus::Unavailable) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.55f, 0.55f, 1.f), "%s", wa::ui_text::get(wa::ui_text::kPipelineEtwUnavailable));
    } else if (etwStatus == wa::EtwWatchStatus::Listening) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kPipelineEtwListening));
    }
    const auto choices = std::count_if(pipelineSessions_.begin(), pipelineSessions_.end(),
        [this](const auto& row) { return pipelineSelectedPid_ && row.processId == pipelineSelectedPid_; });
    std::string graphPreview = wa::ui_text::get(wa::ui_text::kPipelineChooseSession);
    const auto graphLabel = [](const wa::LiveSessionView& row) {
        return row.processName + " | PID " + std::to_string(row.processId) + " | " +
            wa::ui_text::get(row.flow == wa::PipelineFlow::Capture
                ? wa::ui_text::kPipelineFlowCapture : wa::ui_text::kPipelineFlowRender) +
            " | " + row.deviceName + " | " + row.state;
    };
    if (pipelineSelected_ >= 0 && pipelineSelected_ < static_cast<int>(pipelineSessions_.size()))
        graphPreview = graphLabel(pipelineSessions_[static_cast<size_t>(pipelineSelected_)]);
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kGraphTarget));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##graphSession", graphPreview.c_str())) {
        for (size_t i = 0; i < pipelineSessions_.size(); ++i) {
            const auto& row = pipelineSessions_[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(graphLabel(row).c_str(), pipelineSelected_ == static_cast<int>(i))) {
                pipelineSelectedPid_ = row.processId;
                pipelineSelected_ = static_cast<int>(i);
                pipelineProbes_.clear();
                rebuildPipelineGraph();
            }
            ImGui::SetItemTooltip("%s", wa::ui_text::formatLiveSessionTooltip(row).c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    if (pipelineSelected_ < 0) {
        ImGui::TextWrapped("%s", wa::ui_text::get(pipelineSelectedPid_
            ? choices ? wa::ui_text::kPipelineChooseSession : wa::ui_text::kPipelineNoSession
            : wa::ui_text::kPipelineSelectTarget));
    } else {
        const auto& session = pipelineSessions_[static_cast<size_t>(pipelineSelected_)];
        ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelineGraphSourceFmt),
                    session.processName.c_str(), session.processId);
        ImGui::SetItemTooltip("%s", wa::ui_text::formatLiveSessionTooltip(session).c_str());
        if (choices == 1) {
            continueWorkspaceRow(260.f);
            ImGui::TextDisabled("%s | %s", wa::ui_text::get(session.flow == wa::PipelineFlow::Capture
                ? wa::ui_text::kPipelineFlowCapture : wa::ui_text::kPipelineFlowRender), session.deviceName.c_str());
        }
        const auto* source = pipelineCallStore_.segment(pipelineCallStore_.latestAttach(
            wa::ProcessInstance{session.processId, session.processCreated}));
        if (source) {
            const auto status = source->state == wa::CallLogStore::SegmentState::Monitoring
                ? wa::ui_text::kPipelineMonitoring : source->state == wa::CallLogStore::SegmentState::Exited
                ? wa::ui_text::kPipelineExited : wa::ui_text::kPipelineDetachedHistory;
            ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelineGraphSegmentFmt),
                static_cast<unsigned long long>(source->id), wa::ui_text::get(status));
        } else if (!session.processCreated) {
            ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineIdentityUnknown));
        }
        {
            const char* initBanner = wa::ui_text::get(wa::ui_text::kPipelineInitSourceNone);
            ImVec4 initColor(0.90f, 0.55f, 0.55f, 1.f);
            if (pipelineJoinStatus_.initializeSource == wa::InitializeFieldSource::Etw) {
                initBanner = wa::ui_text::get(wa::ui_text::kPipelineInitSourceEtw);
                initColor = ImVec4(0.50f, 0.75f, 0.95f, 1.f);
            } else if (pipelineJoinStatus_.initializeSource ==
                       wa::InitializeFieldSource::Hooked) {
                initBanner = wa::ui_text::get(wa::ui_text::kPipelineInitSourceHooked);
                initColor = ImVec4(0.45f, 0.90f, 0.55f, 1.f);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, initColor);
            ImGui::TextWrapped("%s", initBanner);
            ImGui::PopStyleColor();
        }
        if (pipelineJoinStatus_.probesPresent) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.35f, 1.f));
            ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineProbeBanner));
            ImGui::PopStyleColor();
        }
        if (pipelineJoinStatus_.hookedStreamCount > 1) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.35f, 1.f));
            ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineMultiStreamBanner));
            ImGui::PopStyleColor();
            if (pipelineJoinStatus_.primaryStreamId != 0)
                ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelinePrimaryStreamFmt),
                            pipelineJoinStatus_.primaryStreamId);
        } else if (pipelineJoinStatus_.initializeSource == wa::InitializeFieldSource::Hooked &&
                   pipelineJoinStatus_.primaryStreamId != 0) {
            ImGui::Text(wa::ui_text::get(wa::ui_text::kPipelinePrimaryStreamFmt),
                        pipelineJoinStatus_.primaryStreamId);
        }
        if (pipelineJoinStatus_.otherPidCalls > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.75f, 0.95f, 1.f));
            ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kPipelineOtherPidBanner));
            ImGui::PopStyleColor();
        }
        const auto& nodes = pipelineGraph_.nodes();
        if (!nodes.empty() && ImGui::BeginTable("pipelineNodeGraph", static_cast<int>(nodes.size()),
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchSame,
                ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y)),
                std::max(ImGui::GetContentRegionAvail().x, static_cast<float>(nodes.size()) * 130.f))) {
            for (const auto& n : nodes) ImGui::TableSetupColumn(n.id.c_str());
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableNextRow();
            for (size_t i = 0; i < nodes.size(); ++i) {
                const auto& n = nodes[i];
                ImGui::TableSetColumnIndex(static_cast<int>(i));
                ImGui::TextWrapped("%zu. %s%s", i + 1, n.title.c_str(), i + 1 < nodes.size() ? " >" : "");
                ImGui::TextColored(kindColor(n.kind), "%s", wa::observationKindName(n.kind));
                ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kPipelineKindLegend));
            }
            ImGui::TableNextRow();
            for (size_t i = 0; i < nodes.size(); ++i) {
                ImGui::TableSetColumnIndex(static_cast<int>(i));
                for (const auto& param : nodes[i].params) {
                    ImGui::TextDisabled("%s", param.key.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text, kindColor(param.kind));
                    ImGui::TextWrapped("%s [%s]", param.value.c_str(), wa::observationKindName(param.kind));
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                }
            }
            ImGui::EndTable();
        }
    }
}

void AppUi::beginDumpPick(DumpPickKind kind, wa::TrackId trackId) {
    if (dumpPicker_.busy()) return;
    dumpPickKind_ = kind;
    dumpPickTrackId_ = trackId;
    if (!dumpPicker_.start(wa::dump_ui::loadDumpFolder())) {
        dumpPickKind_ = DumpPickKind::None;
        dumpPickTrackId_ = 0;
    }
}

void AppUi::applyDumpPick() {
    std::wstring folder;
    bool accepted = false;
    if (!dumpPicker_.take(folder, accepted)) return;
    const DumpPickKind kind = dumpPickKind_;
    const wa::TrackId tid = dumpPickTrackId_;
    dumpPickKind_ = DumpPickKind::None;
    dumpPickTrackId_ = 0;
    if (!accepted || kind == DumpPickKind::None) return;
    wa::dump_ui::saveDumpFolder(folder);

    auto logStart = [&](wa::Result r, const std::wstring& path) {
        logLines_.push_back(r ? ("dump started " + wtou(path))
                              : ("dump start error: " + r.message));
    };
    if (kind == DumpPickKind::Loopback || kind == DumpPickKind::AppLoopback) {
        wa::CaptureTrackList& list =
            (kind == DumpPickKind::Loopback) ? loopbackTracks_ : appLoopbackTracks_;
        wa::Result r = list.startDump(tid, folder);
        std::wstring path;
        for (const auto& s : list.poll())
            if (s.id == tid) path = s.dumpPath;
        logStart(r, path);
        return;
    }
    if (kind == DumpPickKind::MonitorCap) {
        ms_ = monitor_.poll();
        consumeMonitorDumps();
        wa::Result r = monitor_.startDumpCapture(folder);
        logStart(r, monitor_.poll().capDumpPath);
        return;
    }
    if (kind == DumpPickKind::MonitorRen) {
        ms_ = monitor_.poll();
        consumeMonitorDumps();
        wa::Result r = monitor_.startDumpRender(folder);
        logStart(r, monitor_.poll().renderDumpPath);
    }
}

void AppUi::consumeMonitorDumps() {
    const auto consume = [this](wa::dump_ui::Completion& completion,
                                const wa::WavSinkStatus& dump) {
        if (!completion.take(dump)) return;
        logLines_.push_back("dump completed generation=" + std::to_string(dump.generation) +
                            " " + wtou(dump.path));
        wa::dump_ui::revealDumpFile(dump.path);
    };
    consume(capDumpCompletion_, ms_.capDump);
    consume(renderDumpCompletion_, ms_.renderDump);
}

void AppUi::drawMonitorDumpControls(const wa::WavSinkStatus& dump, bool sourceAvailable,
                                     DumpPickKind kind, const char* caption) {
    ImGui::PushID(static_cast<int>(kind));
    if (dump.phase == wa::WavDumpPhase::Accepting) {
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kDumpStop))) {
            const auto r = kind == DumpPickKind::MonitorCap ? monitor_.stopDumpCapture()
                                                           : monitor_.stopDumpRender();
            if (!r) logLines_.push_back("dump stop error: " + r.message);
            ms_ = monitor_.poll();
            consumeMonitorDumps();
        }
    } else {
        ImGui::BeginDisabled(!wa::dump_ui::canStart(dump, sourceAvailable, dumpPicker_.busy()));
        if (ImGui::Button(wa::ui_text::id(caption))) beginDumpPick(kind);
        ImGui::EndDisabled();
    }
    if (dump.phase != wa::WavDumpPhase::Idle) {
        ImGui::SameLine();
        ImGui::TextUnformatted(wa::dump_ui::statusText(dump.phase));
        if (!dump.fileName.empty()) ImGui::TextDisabled("%s", wtou(dump.fileName).c_str());
        if (dump.phase == wa::WavDumpPhase::Failure && !dump.message.empty())
            ImGui::TextWrapped("%s", dump.message.c_str());
    }
    ImGui::PopID();
}

bool AppUi::drawDumpControls(wa::CaptureTrackList& list, const wa::CaptureTrackStatus& t,
                             DumpPickKind kind, const char* destroyedLog) {
    const bool picking = dumpPicker_.busy();
    if (t.dumping) {
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kDumpStop))) {
            const std::wstring path = t.dumpPath;
            wa::Result r = list.stopDump(t.id);
            logLines_.push_back(r ? ("dump stopped " + wtou(path))
                                  : ("dump stop error: " + r.message));
            if (r) wa::dump_ui::revealDumpFile(path);
        }
    } else {
        ImGui::BeginDisabled(t.state != wa::StreamState::Running || picking);
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kDump)))
            beginDumpPick(kind, t.id);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kLoopbackDestroy))) {
        const std::wstring dumpPath = t.dumping ? t.dumpPath : std::wstring{};
        list.destroy(t.id);
        if (!dumpPath.empty()) wa::dump_ui::revealDumpFile(dumpPath);
        logLines_.push_back(std::string(destroyedLog) + std::to_string(t.id));
        return true;
    }
    if (t.dumping && !t.dumpFileName.empty()) {
        ImGui::TextWrapped("%s", wtou(t.dumpFileName).c_str());
    }
    return false;
}

static wa::DeviceId deviceIdAt(const std::vector<wa::DeviceInfo>& devs, int idx) {
    if (idx < 0 || idx >= (int)devs.size()) return {};
    return devs[(size_t)idx].id;
}

void AppUi::recomputeRenderFormat() {
    wa::ComInitGuard com;
    wa::DeviceCapabilities caps;
    const wa::DeviceId id = deviceIdAt(renderDevices_, renderPageDevIdx_);
    wa::Result result = enumerator_.queryCapabilities(wa::DataFlow::Render, id, caps);
    if (!result) {
        logLines_.push_back("render capabilities error: " + result.message);
        caps = wa::DeviceCapabilities{};
    }
    const bool changed = wa::create_recipe::updateFormatDevice(
        renderRecipe_, renderPageDevIdx_, id, caps, caps.mixFormat);
    if (changed) {
        renderLayoutIdx_ = 0;
        renderHaveCustom_ = false;
    }
}

void AppUi::drawRenderPage() {
    const float logH = logRegionHeight();
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - logH - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("renderTop", ImVec2(0, topHeight), false);
    const float configSpan = std::max(1.f, ImGui::GetContentRegionAvail().x - wa::pipeline_split::kSplitterPx);
    ImGui::BeginChild("renderLeft", ImVec2(wa::pipeline_split::firstPaneSize(configSpan, renderConfigFrac_, 260.f, 360.f), 0), true);
    drawRenderLeftPanel();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    drawWorkspaceSplitter("##configurationSplit", true, renderConfigFrac_, configSpan);
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("renderTracks", ImVec2(0, 0), true);
#ifdef WA_PERFORMANCE_FIXTURE
    PerformanceFixture::scrollCharts(*this);
#endif
    auto tracks = renderTracks_.poll();
    ImGui::Text("%s (%zu)", wa::ui_text::get(wa::ui_text::kLoopbackTracks), tracks.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(tracks.empty());
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kLoopbackDestroyAll))) {
        renderTracks_.destroyAll();
        renderControlErrors_.clear();
        tracks.clear();
        logLines_.push_back("render destroy all");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", wa::ui_text::get(wa::ui_text::kRemoveTrackHint));
    ImGui::Separator();
    for (auto it = renderControlErrors_.begin(); it != renderControlErrors_.end();) {
        if (std::none_of(tracks.begin(), tracks.end(), [&](const auto& t) { return t.id == it->first; }))
            it = renderControlErrors_.erase(it);
        else ++it;
    }
    std::vector<uint64_t> live;
    for (const auto& t : tracks) live.push_back(t.id);
    renderView_.reconcile(live);
    renderViz_.erase(std::remove_if(renderViz_.begin(), renderViz_.end(), [&](const auto& e) {
        return std::find(live.begin(), live.end(), e.first) == live.end();
    }), renderViz_.end());
    if (tracks.empty()) ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kRenderEmptyHint));
    else {
        drawTrackViewToolbar(renderView_);
        ImGui::SameLine();
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kResetLayout))) { renderView_.mode = 0; renderConfigFrac_ = 0.27f; }
        std::vector<uint64_t> removed;
        const float tableH = std::min(180.f, ImGui::GetContentRegionAvail().y * 0.28f);
        if (ImGui::BeginTable("renderSummary", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(0, std::max(75.f, tableH)))) {
            ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kTrackCaption), ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kRenderEndpoint), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kStatus), ImGuiTableColumnFlags_WidthFixed, 100.f);
            ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kTrackActions), ImGuiTableColumnFlags_WidthFixed, 145.f);
            ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kCompare), ImGuiTableColumnFlags_WidthFixed, 50.f);
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            for (const auto& t : tracks) {
                ImGui::PushID(static_cast<int>(t.id));
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(std::to_string(t.id).c_str(), renderView_.selected == t.id)) renderView_.select(t.id);
                ImGui::TableSetColumnIndex(1);
                std::string name = wtou(t.deviceId);
                for (const auto& d : renderDevices_) if (d.id == t.deviceId) { name = wtou(d.name); break; }
                ImGui::TextWrapped("%s", name.c_str());
                ImGui::SetItemTooltip("%s", wtou(t.deviceId).c_str());
                ImGui::TextWrapped("%s: %s", wa::ui_text::get(wa::ui_text::kClientFormat), formatLabel(t.clientFormat).c_str());
                ImGui::TextWrapped("%s: %s", wa::ui_text::get(wa::ui_text::kActualFormat), t.actualFormat.sampleRate ? formatLabel(t.actualFormat).c_str() : wa::ui_text::get(wa::ui_text::kUnavailableFormat));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(streamStateText(t.state));
                const bool allPaused = !t.channelPaused.empty() && std::all_of(t.channelPaused.begin(), t.channelPaused.end(), [](uint8_t p) { return p != 0; });
                ImGui::TextWrapped("%s", wa::ui_text::get(allPaused ? wa::ui_text::kRenderLoopPaused : wa::ui_text::kRenderLoopActive));
                drawOperationError(t.message);
                ImGui::TableSetColumnIndex(3);
                if (ImGui::Button(wa::ui_text::id(wa::ui_text::kLoopbackDestroy))) {
                    renderTracks_.destroy(t.id); removed.push_back(t.id);
                    renderControlErrors_.erase(t.id);
                    logLines_.push_back("render track destroyed id=" + std::to_string(t.id));
                }
                ImGui::TableSetColumnIndex(4);
                bool compare = renderView_.compared == t.id;
                ImGui::BeginDisabled(renderView_.selected == t.id);
                if (ImGui::Checkbox("##compare", &compare)) { renderView_.compared = compare ? t.id : 0; if (compare) renderView_.mode = 1; }
                ImGui::EndDisabled(); ImGui::PopID();
            }
            ImGui::EndTable();
        }
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const auto& t) {
            return std::find(removed.begin(), removed.end(), t.id) != removed.end();
        }), tracks.end());
        live.clear(); for (const auto& t : tracks) live.push_back(t.id); renderView_.reconcile(live);
        const bool compare = renderView_.mode == 1 && renderView_.compared;
        const float width = compare ? std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f) : 0.f;
        int drawn = 0;
        for (const auto& t : tracks) {
            if (!renderView_.visible(t.id)) continue;
            if (compare && drawn++) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(t.id));
            ImGui::BeginChild("renderDetail", ImVec2(width, renderView_.mode == 2 ? 650.f : 0.f), true);
            ImGui::Text("%s %llu | %s", wa::ui_text::get(wa::ui_text::kTrackCaption), static_cast<unsigned long long>(t.id), streamStateText(t.state));
            const bool sideBySide = ImGui::GetContentRegionAvail().x >= 660.f;
            const float controlsW = sideBySide ? ImGui::GetContentRegionAvail().x * 0.55f : 0.f;
            ImGui::BeginChild("channelControls", ImVec2(controlsW, sideBySide ? 0.f : ImGui::GetFrameHeightWithSpacing() * 11.f), true);
            if (t.state == wa::StreamState::Running) {
                const auto showControlResult = [&](const wa::Result& result) {
                    renderControlErrors_[t.id] = result ? std::string{} : result.message;
                    if (!result) logLines_.push_back("render channel error: " + result.message);
                };
                if (ImGui::BeginTable("channels", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
                                                    | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kChannelCaption), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kChannelLoop), ImGuiTableColumnFlags_WidthFixed, 88);
                    ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kRenderPlayOnce), ImGuiTableColumnFlags_WidthFixed, 92);
                    ImGui::TableSetupColumn(wa::ui_text::get(wa::ui_text::kRenderVolume), ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableHeadersRow();
                    for (uint16_t channel = 0; channel < t.channels.size(); ++channel) {
                        ImGui::PushID(channel);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const wa::ChannelPhrase phrase = t.channels[channel].phrase;
                        const char* name = phrase == wa::ChannelPhrase::None
                            ? wa::ui_text::get(wa::ui_text::kRenderNoPhrase) : wa::ui_text::channelPhraseText(phrase);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextWrapped("%u / %s", static_cast<unsigned>(channel + 1), name);
                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Button(wa::ui_text::id(t.channels[channel].paused
                                ? wa::ui_text::kRenderContinue : wa::ui_text::kRenderPause), ImVec2(-1, 0)))
                            showControlResult(t.channels[channel].paused
                                ? renderTracks_.continueChannel(t.id, channel) : renderTracks_.pauseChannel(t.id, channel));
                        ImGui::TableSetColumnIndex(2);
                        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kRenderPlayOnce), ImVec2(-1, 0)))
                            showControlResult(renderTracks_.playOnce(t.id, channel));
                        ImGui::TableSetColumnIndex(3);
                        int volume = t.channels[channel].volumePercent;
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::SliderInt("##volume", &volume, 0, 100, "%d%%"))
                            showControlResult(renderTracks_.setVolume(t.id, channel, volume));
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                drawOperationError(renderControlErrors_[t.id]);
            }
            ImGui::EndChild();
            if (sideBySide) ImGui::SameLine();
            ImGui::BeginChild("renderCharts", ImVec2(0, sideBySide ? 0.f : 340.f), true);
            auto it = std::find_if(renderViz_.begin(), renderViz_.end(), [&](const auto& e) { return e.first == t.id; });
            if (it == renderViz_.end()) { renderViz_.push_back({t.id, VisualState{}}); it = renderViz_.end() - 1; }
#ifdef WA_PERFORMANCE_FIXTURE
            PerformanceFixture::scrollChartPanel(*this, t.id);
#endif
            wa::MonitorStatus chartStatus{};
            chartStatus.overall = t.state; chartStatus.capState = t.state;
            chartStatus.sampleRate = t.actualFormat.sampleRate ? t.actualFormat.sampleRate : t.clientFormat.sampleRate;
            chartStatus.captureChannels = t.chartChannels;
            wa::RenderTrackScopeReader reader(renderTracks_, t.id);
            it->second.probeSource = t.id; it->second.probeDirection = 1;
            drawChartHost(nullptr, chartStatus, it->second, wa::chart_host::Mode::CaptureOnly, nullptr, nullptr, &reader);
            ImGui::EndChild(); ImGui::EndChild(); ImGui::PopID();
        }
    }
    ImGui::EndChild(); ImGui::EndChild();
    drawLogRegion("renderLogRegion", "renderLog");
}

void AppUi::drawRenderLeftPanel() {
    if (!monitorDevicesLoaded_) refreshMonitorDevices();

    ImGui::BeginChild("configuration", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 4.0f), false);
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kRenderEndpoint));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kRefreshDevices))) refreshMonitorDevices();
    std::string preview = wa::ui_text::get(wa::ui_text::kNoRenderDevices);
    if (!renderDevices_.empty() && renderPageDevIdx_ >= 0 &&
        renderPageDevIdx_ < (int)renderDevices_.size()) {
        preview = devicePickerLabel(renderDevices_[(size_t)renderPageDevIdx_], true);
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##renderEndpoint", preview.c_str())) {
        for (int i = 0; i < (int)renderDevices_.size(); ++i) {
            const std::string label = devicePickerLabel(renderDevices_[(size_t)i], false) +
                                      "##renderDev" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), renderPageDevIdx_ == i))
                renderPageDevIdx_ = i;
        }
        ImGui::EndCombo();
    }

    if (renderRecipe_.deviceShown != renderPageDevIdx_)
        recomputeRenderFormat();

    wa::AudioFormat mix{};
    mix.channels = 0;
    if (renderRecipe_.caps.hasMix)
        mix = renderRecipe_.caps.mixFormat;

    const std::vector<wa::RenderLayout> layouts = wa::collapseSharedLayouts(
        wa::create_recipe::sharedCandidates(renderRecipe_.caps));
    if (renderLayoutIdx_ > (int)layouts.size()) renderLayoutIdx_ = 0;

    // Chosen layout for the combo preview and for Create. Called again after the
    // widgets so Create sees a selection made earlier in this frame.
    auto chosenLayout = [&]() {
        wa::RenderTrackCreate spec;
        spec.mode = wa::RenderLayoutMode::SystemDefault;
        spec.source = mix;
        std::string label = wa::ui_text::get(wa::ui_text::kSystemDefault);
        if (renderHaveCustom_ && renderLayoutIdx_ < 0) {
            spec.mode = wa::RenderLayoutMode::Layout;
            spec.source = wa::AudioFormat{};
            spec.source.channels = renderCustomLayout_.channels;
            spec.source.channelMask = renderCustomLayout_.channelMask;
            spec.source.sampleRate = 0;
            spec.source.bitsPerSample = 0;
            label = wa::ui_text::channelLayoutText(spec.source.channels, spec.source.channelMask);
        } else if (renderLayoutIdx_ >= 1 && renderLayoutIdx_ <= (int)layouts.size()) {
            const wa::RenderLayout& layout = layouts[(size_t)renderLayoutIdx_ - 1];
            spec.mode = wa::RenderLayoutMode::Layout;
            spec.source = wa::AudioFormat{};
            spec.source.channels = layout.channels;
            spec.source.channelMask = layout.channelMask;
            spec.source.sampleRate = 0;
            spec.source.bitsPerSample = 0;
            label = wa::ui_text::channelLayoutText(layout.channels, layout.channelMask);
        }
        return std::pair<wa::RenderTrackCreate, std::string>(std::move(spec), std::move(label));
    };

    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kRenderLayout));
    ImGui::TextDisabled(wa::ui_text::get(wa::ui_text::kRenderFormatUnits), wa::kRenderClientRate,
                        static_cast<unsigned>(wa::kRenderClientBits));

    const std::string layoutPreview = chosenLayout().second;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##renderLayout", layoutPreview.c_str())) {
        const std::string defLabel = std::string(wa::ui_text::get(wa::ui_text::kSystemDefault)) + "###rl0";
        if (ImGui::Selectable(defLabel.c_str(), renderLayoutIdx_ == 0 && !renderHaveCustom_)) {
            renderLayoutIdx_ = 0;
            renderHaveCustom_ = false;
        }
        for (int i = 0; i < (int)layouts.size(); ++i) {
            const std::string label =
                wa::ui_text::channelLayoutText(layouts[(size_t)i].channels,
                                               layouts[(size_t)i].channelMask) +
                "##rl" + std::to_string(i + 1);
            if (ImGui::Selectable(label.c_str(), renderLayoutIdx_ == i + 1)) {
                renderLayoutIdx_ = i + 1;
                renderHaveCustom_ = false;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kCustomChannels));
    const float applyWidth = ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kApply)).x
                           + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(-applyWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##renderCustom", renderCustom_, sizeof(renderCustom_));
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kApply))) {
        renderFormatError_.clear();
        const wa::RenderCustomLayout layout = wa::renderLayoutFromCustom(std::string("48000/16/") + renderCustom_);
        if (!layout.ok) {
            renderFormatError_ = layout.message.empty() ? "invalid format" : layout.message;
            logLines_.push_back(renderFormatError_);
        } else {
            renderCustomLayout_ = layout;
            renderHaveCustom_ = true;
            renderLayoutIdx_ = -1;
        }
    }
    drawOperationError(renderFormatError_);
    ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kFixedRenderFormat));

    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kRenderCreateHint));
    const ImVec2 ctrlBtn(-1, ImGui::GetFrameHeight() * 1.3f);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kLoopbackCreate), ctrlBtn)) {
        wa::RenderTrackCreate spec = chosenLayout().first;
        spec.deviceId = deviceIdAt(renderDevices_, renderPageDevIdx_);
        wa::TrackId tid = 0;
        wa::Result r = renderTracks_.create(spec, &tid);
        renderCreateError_ = r ? std::string{} : r.message;
        logLines_.push_back(r ? ("render track created id=" + std::to_string(tid))
                              : ("render error: " + r.message));
    }
    drawOperationError(renderCreateError_);
}

void AppUi::drawLoopbackLeftPanel() {
    if (!monitorDevicesLoaded_) refreshMonitorDevices();

    ImGui::BeginChild("configuration", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3.6f), false);
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kSource));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kRefreshDevices))) refreshMonitorDevices();

    std::string preview = wa::ui_text::get(wa::ui_text::kNoRenderDevices);
    if (!renderDevices_.empty() && loopbackDevIdx_ >= 0 && loopbackDevIdx_ < (int)renderDevices_.size())
        preview = devicePickerLabel(renderDevices_[(size_t)loopbackDevIdx_], true);
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kSystemAudio));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##systemAudio", preview.c_str())) {
        for (int i = 0; i < (int)renderDevices_.size(); ++i) {
            std::string l = devicePickerLabel(renderDevices_[(size_t)i], false);
            if (ImGui::Selectable((l + "##loopdev" + std::to_string(i)).c_str(),
                                  loopbackDevIdx_ == i))
                loopbackDevIdx_ = i;
        }
        ImGui::EndCombo();
    }

    if (loopbackRecipe_.deviceShown != loopbackDevIdx_)
        recomputeLoopbackFormat();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kCapabilities))) {
        capsModal_ = loopbackRecipe_.caps;
        capsModalContext_ = wa::ui_text::kSystemLoopbackRenderDevice;
        capsDeviceSummary_ = preview + "\n" + wtou(loopbackRecipe_.deviceId);
        capsSelected_ = -1;
        ImGui::OpenPopup(wa::ui_text::id(wa::ui_text::kDeviceCapabilities));
    }
    drawFormatRecipe(loopbackRecipe_.format,
                     wa::create_recipe::sharedFormatChoices(loopbackRecipe_.caps),
                     loopbackRecipe_.caps.mixFormat, loopbackRecipe_.caps.hasMix,
                     wa::ui_text::kSystemDefault, "sysLbFmt", loopbackRecipe_.params);
    drawCapsModal();
    ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kLoopbackSilentRender), &loopbackSilentRender_);

    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kNewTrackHint));
    const ImVec2 ctrlBtn(-1, ImGui::GetFrameHeight() * 1.3f);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kCreateCapture), ctrlBtn)) {
        wa::DeviceId id = renderDevices_.empty() ? L"" : renderDevices_[(size_t)loopbackDevIdx_].id;
        wa::CaptureTrackCreate spec{};
        spec.kind = wa::BackendKind::WasapiShared;
        spec.source = wa::CaptureSource{wa::CaptureSourceKind::SystemLoopback, id};
        spec.loopbackOptions.silentRender = loopbackSilentRender_;
        spec.streamParams = loopbackRecipe_.params;
        spec.requested = wa::create_recipe::requestedOrNull(loopbackRecipe_.format);
        wa::TrackId tid = 0;
        wa::Result r = loopbackTracks_.create(spec, &tid);
        loopbackCreateError_ = r ? std::string{} : r.message;
        logLines_.push_back(r ? ("loopback track created id=" + std::to_string(tid))
                              : ("loopback error: " + r.message));
    }
    drawOperationError(loopbackCreateError_);
}

void AppUi::drawApplicationLoopbackLeftPanel() {
    if (!monitorDevicesLoaded_)
        refreshMonitorDevices();
    if (!appLoopbackSessionsLoaded_)
        refreshApplicationLoopbackSessions();

    ImGui::BeginChild("configuration", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 6.6f), false);
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kTargetProcess));
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kApplicationLoopbackPidLabel));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##appLoopbackPid", appLoopbackPid_, sizeof(appLoopbackPid_))) {
        appLoopbackSessionIdx_ = -1;
        appLoopbackCreateError_.clear();
    }
    int scope = appLoopbackExclude_ ? 1 : 0;
    ImGui::RadioButton(wa::ui_text::id(wa::ui_text::kCaptureTree), &scope, 0);
    ImGui::RadioButton(wa::ui_text::id(wa::ui_text::kExcludeTree), &scope, 1);
    appLoopbackExclude_ = scope == 1;
    ImGui::TextWrapped("%s", wa::ui_text::get(appLoopbackExclude_
        ? wa::ui_text::kExcludeTreeHint : wa::ui_text::kIncludeTreeHint));
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kApplicationLoopbackSessions));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kApplicationLoopbackRefresh)))
        refreshApplicationLoopbackSessions();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sessionSearch", wa::ui_text::get(wa::ui_text::kSessionSearch),
                             appLoopbackFilter_, sizeof(appLoopbackFilter_));
    ImGui::BeginChild("appLoopbackSessionList", ImVec2(0, appSessionListHeight_), true);
    bool any = false;
    for (int i = 0; i < static_cast<int>(appLoopbackSessions_.size()); ++i) {
        const auto& row = appLoopbackSessions_[static_cast<size_t>(i)];
        const std::string label = wtou(row.processName) + "  /  " + std::to_string(row.processId);
        if (!wa::call_log_filter::fuzzyMatch(label, appLoopbackFilter_)) continue;
        any = true;
        ImGui::PushID(i);
        uint32_t selectedPid = 0;
        const bool selected = wa::parseApplicationLoopbackPid(appLoopbackPid_, selectedPid)
                              && selectedPid == row.processId;
        if (ImGui::Selectable(label.c_str(), selected)) {
            appLoopbackSessionIdx_ = i;
            appLoopbackCreateError_.clear();
            wa::app_loopback_ui::copySessionPidToBuffer(appLoopbackSessions_, i,
                                                       appLoopbackPid_, sizeof(appLoopbackPid_));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label.c_str());
        ImGui::PopID();
    }
    if (!any) ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kNoMatchingSessions));
    ImGui::EndChild();
    ImGui::InvisibleButton("##sessionHeight", ImVec2(-1, 6.f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) appSessionListHeight_ = std::clamp(appSessionListHeight_ + ImGui::GetIO().MouseDelta.y, 65.f, 280.f);
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kCaptureFormatTab));
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kFormatReference));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kRefreshDevices)))
        refreshMonitorDevices();

    std::string referencePreview = wa::ui_text::get(wa::ui_text::kFormatReferenceNone);
    if (appLoopbackReferenceIdx_ == 1) {
        referencePreview = wa::ui_text::get(wa::ui_text::kFormatReferenceDefaultRender);
    } else if (appLoopbackReferenceIdx_ >= 2 &&
               appLoopbackReferenceIdx_ - 2 < (int)renderDevices_.size()) {
        const auto& device = renderDevices_[static_cast<size_t>(appLoopbackReferenceIdx_ - 2)];
        referencePreview = devicePickerLabel(device, true);
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##appLoopbackFormatReference", referencePreview.c_str())) {
        if (ImGui::Selectable(wa::ui_text::id(wa::ui_text::kFormatReferenceNone),
                              appLoopbackReferenceIdx_ == 0))
            appLoopbackReferenceIdx_ = 0;
        if (ImGui::Selectable(wa::ui_text::id(wa::ui_text::kFormatReferenceDefaultRender),
                              appLoopbackReferenceIdx_ == 1))
            appLoopbackReferenceIdx_ = 1;
        for (int i = 0; i < (int)renderDevices_.size(); ++i) {
            const auto& device = renderDevices_[static_cast<size_t>(i)];
            const std::string label = devicePickerLabel(device, false) +
                                      "##appref" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), appLoopbackReferenceIdx_ == i + 2))
                appLoopbackReferenceIdx_ = i + 2;
        }
        ImGui::EndCombo();
    }
    if (appLoopbackRecipe_.deviceShown != appLoopbackReferenceIdx_)
        recomputeApplicationLoopbackFormat();
    ImGui::BeginDisabled(appLoopbackReferenceIdx_ == 0);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kCapabilities))) {
        capsModal_ = appLoopbackRecipe_.caps;
        capsModalContext_ = wa::ui_text::kApplicationLoopbackFormatReference;
        capsDeviceSummary_ = referencePreview + "\n" + wtou(appLoopbackRecipe_.deviceId);
        capsSelected_ = -1;
        ImGui::OpenPopup(wa::ui_text::id(wa::ui_text::kDeviceCapabilities));
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kReferenceOnly));
    if (appLoopbackReferenceIdx_ == 0)
        ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kFormatReferenceNone));
    drawFormatRecipe(appLoopbackRecipe_.format,
                     wa::create_recipe::sharedFormatChoices(appLoopbackRecipe_.caps),
                     wa::AudioFormat{}, false, wa::ui_text::kAutomatic, "appLbFmt",
                     appLoopbackRecipe_.params);
    drawCapsModal();

    ImGui::EndChild();
    ImGui::Separator();
    std::string targetName;
    uint32_t targetPid = 0;
    if (wa::parseApplicationLoopbackPid(appLoopbackPid_, targetPid))
        for (const auto& row : appLoopbackSessions_) if (row.processId == targetPid) { targetName = wtou(row.processName); break; }
    ImGui::TextWrapped("%s | PID %s", targetName.c_str(), appLoopbackPid_);
    ImGui::TextWrapped("%s | %s", wa::ui_text::get(appLoopbackExclude_ ? wa::ui_text::kExcludeTree : wa::ui_text::kCaptureTree),
        appLoopbackRecipe_.format.haveRequested ? formatLabel(appLoopbackRecipe_.format.selected).c_str() : wa::ui_text::get(wa::ui_text::kAutomatic));
    ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kNewTrackHint));
    const ImVec2 ctrlBtn(-1, ImGui::GetFrameHeight() * 1.3f);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kCreateCapture), ctrlBtn)) {
        uint32_t pid = 0;
        if (!wa::parseApplicationLoopbackPid(appLoopbackPid_, pid)) {
            appLoopbackCreateError_ = wa::ui_text::get(wa::ui_text::kInvalidTargetPid);
            logLines_.push_back("application loopback create error: invalid PID");
        } else {
            const wa::ProcessLoopbackMode mode = appLoopbackExclude_
                ? wa::ProcessLoopbackMode::ExcludeTree
                : wa::ProcessLoopbackMode::IncludeTree;
            wa::CaptureTrackCreate spec{};
            spec.kind = wa::BackendKind::WasapiShared;
            spec.source = wa::CaptureSource{wa::CaptureSourceKind::ApplicationLoopback, L"",
                                            pid, mode};
            spec.streamParams = appLoopbackRecipe_.params;
            spec.requested = wa::create_recipe::requestedOrNull(appLoopbackRecipe_.format);
            wa::TrackId tid = 0;
            wa::Result r = appLoopbackTracks_.create(spec, &tid);
            appLoopbackCreateError_ = r ? std::string{} : r.message;
            logLines_.push_back(r
                ? ("application loopback track created id=" + std::to_string(tid)
                   + " pid=" + std::to_string(pid)
                   + " mode=" + wa::processLoopbackModeName(mode))
                : ("application loopback error: " + r.message));
        }
    }
    drawOperationError(appLoopbackCreateError_);
}

void AppUi::openOsSound(wa::os_sound_ui::Button button) {
    const auto r = wa::os_sound_ui::recipe(button);
    const wchar_t* params = (r.params && r.params[0] != L'\0') ? r.params : nullptr;
    const INT_PTR code = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", r.file, params, nullptr, SW_SHOWNORMAL));
    const std::string args = "file=" + wtou(r.file ? r.file : L"") +
                             " params=" + wtou(params ? params : L"");
    WA_LOG(wa::log::Level::Debug, "OsSoundUi", "ShellExecuteW", args,
           std::to_string(static_cast<long long>(code)));
    if (wa::os_sound_ui::shellExecuteFailed(code)) {
        WA_LOG(wa::log::Level::Warn, "OsSoundUi", "ShellExecuteW", args, r.failureLog);
        logLines_.push_back(wa::ui_text::get(r.failureLog));
    }
}

void AppUi::drawMonitorControls() {
    const ImVec2 ctrlBtn(140.0f, ImGui::GetFrameHeight() * 1.3f);   // enlarged Start/Stop button
    if (!monitorStarted_) {
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kStart), ctrlBtn)) {
            wa::DeviceId capId = capDevices_.empty()    ? L"" : capDevices_[capDevIdx_].id;
            wa::DeviceId renId = renderDevices_.empty() ? L"" : renderDevices_[renderDevIdx_].id;
            wa::BackendKind kind = (backendIdx_ == 1) ? wa::BackendKind::WasapiExclusive
                                                      : wa::BackendKind::WasapiShared;
            wa::Result r = monitor_.start(kind, capId, renId, (uint32_t)delayMs_,
                                          playbackEnabled_, capParams_, renParams_,
                                          haveFmt_ ? &selectedFmt_ : nullptr);
            monitorStartError_ = r ? "" : r.message;
            logLines_.push_back(r ? "monitor started" : ("monitor error: " + r.message));
            if (r) {
                monitorStarted_ = true;
                resetVisuals(monitorViz_);
            }
        }
    } else {
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kStop), ctrlBtn)) {
            monitorStartError_.clear();
            monitor_.stop();
            ms_ = monitor_.poll();
            consumeMonitorDumps();
            monitorStarted_ = false;
            resetVisuals(monitorViz_);
            logLines_.push_back("monitor stopped");
        }
    }
    ImGui::SameLine();
    // vertically center the playback checkbox against the taller Start/Stop button
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ctrlBtn.y - ImGui::GetFrameHeight()) * 0.5f);

    // Playback checkbox — always toggleable; applies live while running, else takes effect on next Start.
    if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kSyncPlayback), &playbackEnabled_)) {
        if (monitorStarted_) monitor_.setPlaybackEnabled(playbackEnabled_);
    }

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(wa::ui_text::get(monitorStarted_ ? wa::ui_text::kRunning : wa::ui_text::kIdle));
    if (!monitorStartError_.empty())
        ImGui::TextWrapped("%s", monitorStartError_.c_str());
    ImGui::TextWrapped("%s: %s | %s: %s | FIFO %.0f ms | %s %llu | xrun %llu / %llu",
        wa::ui_text::get(wa::ui_text::kCaptureDevice),
        wa::ui_text::get(ms_.capState == wa::StreamState::Running ? wa::ui_text::kRunning :
            ms_.capState == wa::StreamState::Error ? wa::ui_text::kError : wa::ui_text::kIdle),
        wa::ui_text::get(wa::ui_text::kRenderDevice),
        wa::ui_text::get(ms_.renderState == wa::StreamState::Running ? wa::ui_text::kRunning :
            ms_.renderState == wa::StreamState::Error ? wa::ui_text::kError : wa::ui_text::kIdle),
        ms_.fifoFillMs, wa::ui_text::get(wa::ui_text::kDriftCorrections),
        static_cast<unsigned long long>(ms_.driftFixes),
        static_cast<unsigned long long>(ms_.capXruns), static_cast<unsigned long long>(ms_.renderXruns));

}

void AppUi::drawLeftPanel() {
    if (!monitorDevicesLoaded_) refreshMonitorDevices();
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kMonitorTools));
    ImGui::BeginDisabled(monitorStarted_);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kRefreshDevices))) refreshMonitorDevices();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kOptions)))
        ImGui::OpenPopup(wa::ui_text::id(wa::ui_text::kAudioParametersAdvanced));
    drawParamsSummary(capParams_);
    drawParamsSummary(renParams_);
    ImGui::BeginDisabled(monitorStarted_);
    const float capsW = ImGui::CalcTextSize(wa::ui_text::get(wa::ui_text::kCaptureCapabilities)).x
                      + ImGui::GetStyle().FramePadding.x * 2.f;
    if (ImGui::GetContentRegionAvail().x > capsW + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kCaptureCapabilities))) {
        wa::ComInitGuard com;
        wa::DeviceId capId = (capDevIdx_ >= 0 && capDevIdx_ < (int)capDevices_.size())
                             ? capDevices_[(size_t)capDevIdx_].id : L"";
        enumerator_.queryCapabilities(wa::DataFlow::Capture, capId, capsCache_);
        capsModal_ = capsCache_;
        capsModalContext_ = wa::ui_text::kMonitorCaptureDevice;
        capsDeviceSummary_ = (capDevIdx_ >= 0 && capDevIdx_ < static_cast<int>(capDevices_.size())
            ? wtou(capDevices_[static_cast<size_t>(capDevIdx_)].name) : wa::ui_text::get(wa::ui_text::kNoDevices)) + "\n" + wtou(capId);
        capsSelected_ = -1;
        ImGui::OpenPopup(wa::ui_text::id(wa::ui_text::kDeviceCapabilities));
    }

    ImGui::EndDisabled();
    drawAdvancedModal();
    drawCapsModal();
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kMonitorConfiguration));
    if (monitorStarted_)
        ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kMonitorLocked));
    const bool anyStreamActive = monitorStarted_ ||
        [&]() {
            for (const auto& t : loopbackTracks_.poll())
                if (t.state == wa::StreamState::Running) return true;
            for (const auto& t : appLoopbackTracks_.poll())
                if (t.state == wa::StreamState::Running) return true;
            for (const auto& t : renderTracks_.poll())
                if (t.state == wa::StreamState::Running) return true;
            return false;
        }();
    StreamFxUi streamFx{
        &streamFxDeviceId_, &streamFxFlow_, &streamFxEffects_,
        &streamFxError_, &streamFxHaveResult_, anyStreamActive,
        [this]() { refreshMonitorDevices(); }};
    auto deviceCombo = [&](const char* caption, const char* comboId,
                           const std::vector<wa::DeviceInfo>& devs, int& idx,
                           wa::DataFlow flow) {
        ImGui::TextUnformatted(wa::ui_text::get(caption));
        std::string preview = wa::ui_text::get(wa::ui_text::kNoDevices);
        if (!devs.empty() && idx >= 0 && idx < (int)devs.size())
            preview = devicePickerLabel(devs[(size_t)idx], true);
        ImGui::BeginDisabled(monitorStarted_);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(comboId, preview.c_str())) {
            for (int i = 0; i < (int)devs.size(); ++i) {
                std::string l = devicePickerLabel(devs[(size_t)i], false);
                if (ImGui::Selectable((l + "##" + std::to_string(i)).c_str(), idx == i))
                    idx = i;
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", preview.c_str());
        ImGui::PushID(comboId);
        if (!devs.empty() && idx >= 0 && idx < (int)devs.size())
            drawDeviceEnhancementsDetail(devs[(size_t)idx], flow, streamFx);
        ImGui::PopID();
    };
    deviceCombo(wa::ui_text::kCaptureDevice, "##capdev", capDevices_, capDevIdx_, wa::DataFlow::Capture);
    ImGui::Spacing();
    deviceCombo(wa::ui_text::kRenderDevice, "##rendev", renderDevices_, renderDevIdx_, wa::DataFlow::Render);
    ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kControl));
    ImGui::BeginDisabled(monitorStarted_);
    const char* backends[] = {
        wa::ui_text::get(wa::ui_text::kWasapiShared),
        wa::ui_text::get(wa::ui_text::kWasapiExclusive),
    };
    ImGui::Combo(wa::ui_text::id(wa::ui_text::kBackend), &backendIdx_, backends, 2);
    ImGui::SliderInt(wa::ui_text::id(wa::ui_text::kDelayMs), &delayMs_, 0, 500);
    drawFormatRegion();

    ImGui::EndDisabled();
    if (ImGui::CollapsingHeader(wa::ui_text::id(wa::ui_text::kMonitorDiagnostics))) {
        const char* ss[] = {
            wa::ui_text::get(wa::ui_text::kIdle),
            wa::ui_text::get(wa::ui_text::kRunning),
            wa::ui_text::get(wa::ui_text::kError),
        };
        ImGui::TextWrapped("overall=%s  cap=%s  ren=%s  sr=%u  delay=%ums",
            ss[(int)ms_.overall], ss[(int)ms_.capState], ss[(int)ms_.renderState],
            ms_.sampleRate, ms_.delayMs);
        ImGui::TextWrapped("fifo=%.0fms  drift=%llu  xrun c/r=%llu/%llu",
            ms_.fifoFillMs,
            (unsigned long long)ms_.driftFixes,
            (unsigned long long)ms_.capXruns,
            (unsigned long long)ms_.renderXruns);
        ImGui::Text("frames c/r=%llu/%llu",
            (unsigned long long)ms_.capWrittenFrames,
            (unsigned long long)ms_.renderWrittenFrames);
    }

}

namespace {

const char* nextUtf8(const char* s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    int n = 1;
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    for (int i = 1; i < n; ++i) {
        if (s[i] == '\0') return s + i;
    }
    return s + n;
}

void drawClippedLogLine(const char* text, float maxWidth, bool disabled) {
    ImGui::AlignTextToFramePadding();
    if (disabled)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

    if (ImGui::CalcTextSize(text).x <= maxWidth) {
        ImGui::TextUnformatted(text);
        if (disabled) ImGui::PopStyleColor();
        return;
    }

    const float ellipsisW = ImGui::CalcTextSize("...").x;
    const float budget = maxWidth - ellipsisW;
    const char* ok = text;
    if (budget > 0.0f) {
        const char* end = text;
        while (*end) {
            const char* next = nextUtf8(end);
            if (ImGui::CalcTextSize(text, next).x > budget) break;
            ok = next;
            end = next;
        }
    }
    ImGui::TextUnformatted(text, ok);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted("...");
    if (disabled) ImGui::PopStyleColor();
}
}  // namespace

float AppUi::logRegionHeight() const {
    if (!logCollapsed_) return std::clamp(logHeight_, 150.f, std::max(150.f, ImGui::GetIO().DisplaySize.y * .50f));
    const ImGuiStyle& st = ImGui::GetStyle();
    return ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f + st.ChildBorderSize * 2.0f;
}

void AppUi::drawLogRegion(const char* regionId, const char* listId) {
    const float height = logRegionHeight();
    if (!logCollapsed_) {
        ImGui::InvisibleButton("##logHeight", ImVec2(-1, 5.f));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
            logHeight_ = std::clamp(height - ImGui::GetIO().MouseDelta.y, 150.f,
                std::max(150.f, ImGui::GetIO().DisplaySize.y * .50f));
    }
    ImGui::BeginChild(regionId, ImVec2(0, height - (logCollapsed_ ? 0.f : 5.f + ImGui::GetStyle().ItemSpacing.y)), true);
    drawLogPanel(listId);
    ImGui::EndChild();
}

static std::wstring exeDirectory() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring w(buf, n);
    const size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos) w.resize(slash);
    return w;
}

void AppUi::ensureLogPrefsPath() {
    if (!logPrefsPath_.empty()) return;
    const std::wstring dir = exeDirectory();
    logPrefsPath_ = dir.empty()
        ? utow(wa::log_region_prefs::kFileName)
        : dir + L"\\" + utow(wa::log_region_prefs::kFileName);
}

void AppUi::loadLogRegionPrefs() {
    ensureLogPrefsPath();
    const auto loaded = wa::log_region_prefs::load(logPrefsPath_);
    logCollapsed_ = loaded.collapsed;
    const auto uiLang = GetUserDefaultUILanguage();
    char langId[8];
    std::snprintf(langId, sizeof(langId), "0x%04X", static_cast<unsigned>(uiLang));
    WA_LOG(wa::log::Level::Debug, "GuiLanguage", "GetUserDefaultUILanguage", "", langId);
    const bool displayChinese = wa::gui_language::uiLanguageIsChinese(uiLang);
    const auto resolved = wa::log_region_prefs::resolve(loaded, displayChinese);
    wa::ui_text::setLanguage(resolved.language);
    WA_LOG(wa::log::Level::Info, "GuiLanguage", "resolve",
           wa::gui_language::token(resolved.language),
           resolved.fromOverride ? "override" : "display");
    if (loaded.kind == wa::log_region_prefs::LoadKind::Invalid) {
        WA_LOG(wa::log::Level::Warn, "LogRegionPrefs", "load",
               wtou(logPrefsPath_), "invalid");
    }
}

void AppUi::loadProductionPhrases() {
    wchar_t exe[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = L"channel-idents";
    if (n > 0 && n < MAX_PATH) {
        dir = (std::filesystem::path(exe).parent_path() / L"channel-idents").wstring();
    }
    std::vector<std::string> missing;
    const wa::Result loaded = wa::loadProductionCatalog(renderPhrases_, dir, &missing);
    if (!loaded) {
        for (const std::string& name : missing)
            logLines_.push_back("channel ident missing: " + name);
    }
}

void AppUi::persistLogRegionPrefs() {
    ensureLogPrefsPath();
    if (!wa::log_region_prefs::save(logPrefsPath_, logCollapsed_)) {
        WA_LOG(wa::log::Level::Warn, "LogRegionPrefs", "save",
               wtou(logPrefsPath_), "failed");
    }
}

void AppUi::drawLogPanel(const char* listId) {
    const bool collapsed = logCollapsed_;
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kLogTitle));
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(collapsed ? wa::ui_text::kLogShow : wa::ui_text::kLogHide))) {
        logCollapsed_ = !collapsed;
        if (!logCollapsed_) logPinToBottomOnExpand_ = true;
        persistLogRegionPrefs();
    }
    if (collapsed) {
        ImGui::SameLine();
        const bool alert = !logLines_.empty() &&
            (wa::log_view::severity(logLines_.back()) >= 3);
        if (alert) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .7f, .3f, 1.f));
        drawClippedLogLine(logLines_.empty() ? wa::ui_text::get(wa::ui_text::kLogEmpty)
            : logLines_.back().c_str(), ImGui::GetContentRegionAvail().x, logLines_.empty());
        if (alert) ImGui::PopStyleColor();
        return;
    }
    continueWorkspaceRow(140.f);
    if (ImGui::SmallButton(wa::ui_text::id(wa::ui_text::kResetLogHeight))) logHeight_ = 200.f;
    continueWorkspaceRow(180.f);
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kRecordLevel));
    ImGui::SameLine();
    static const char* levels[] = {"Trace", "Debug", "Info", "Warn", "Err"};
    ImGui::SetNextItemWidth(85);
    if (ImGui::Combo("##loglevel", &logLevelIdx_, levels, IM_ARRAYSIZE(levels)))
        wa::log::setLevel(static_cast<wa::log::Level>(logLevelIdx_));
    continueWorkspaceRow(230.f);
    ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kDisplayLevel));
    ImGui::SameLine();
    const char* displayLevels[] = {wa::ui_text::get(wa::ui_text::kAllLogLevels), "Info+", "Warn+", "Error"};
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("##displayLevel", &logDisplayLevel_, displayLevels, IM_ARRAYSIZE(displayLevels));
    ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kLogUnclassified));
    continueWorkspaceRow(160.f);
    if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kLogFollow), &logFollow_) && logFollow_)
        logPinToBottomOnExpand_ = true;
    ImGui::SetNextItemWidth(std::min(240.f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputTextWithHint("##logSearch", wa::ui_text::get(wa::ui_text::kLogSearch), logFilter_, sizeof(logFilter_)))
        logPinToBottomOnExpand_ = logFollow_;
    const auto matches = [&](const std::string& line) {
        return wa::log_view::matches(line, logFilter_, logDisplayLevel_);
    };
    continueWorkspaceRow(150.f);
    const bool copy = ImGui::Button(wa::ui_text::id(wa::ui_text::kCopyVisible));
    continueWorkspaceRow(150.f);
    const bool save = ImGui::Button(wa::ui_text::id(wa::ui_text::kSaveLog));
    if (copy || save) {
        std::string content;
        for (const auto& line : logLines_) if (matches(line)) content += line + "\n";
        if (copy) ImGui::SetClipboardText(content.c_str());
        if (save) {
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            const auto path = std::filesystem::path(exeDirectory()) / ("visible-log-" + std::to_string(stamp) + ".txt");
            std::ofstream file(path, std::ios::binary);
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
            file.close();
            logSaveStatus_ = file ? wtou(path.wstring()) : wa::ui_text::get(wa::ui_text::kLogSaveFailed);
        }
    }
    continueWorkspaceRow(100.f);
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kLogClear))) {
        logFirstLine_ += logLines_.size();
        logLines_.clear();
    }
    if (!logSaveStatus_.empty()) ImGui::TextWrapped("%s: %s", wa::ui_text::get(wa::ui_text::kSaveLogResult), logSaveStatus_.c_str());
    if (!ImGui::BeginChild(listId, ImVec2(0, 0), true)) {
        ImGui::EndChild();
        return;
    }
    const bool wasPinned = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    const std::string filterKey = std::to_string(logDisplayLevel_) + "\n" + logFilter_;
    const auto work = logView_.draw(logLines_, logFirstLine_, filterKey, matches);
    frameProbe_.addWork("log-filter-checked", work.checked);
    frameProbe_.addWork("log-height-measured", work.measured);
    frameProbe_.addWork("log-rows-submitted", work.submitted);
    if (logPinToBottomOnExpand_ || (logFollow_ && wasPinned)) ImGui::SetScrollHereY(1);
    logPinToBottomOnExpand_ = false;
    ImGui::EndChild();
}

void AppUi::drawAdvancedModal() {
    prepareAudioModal(850, 600);
    if (!ImGui::BeginPopupModal(wa::ui_text::id(wa::ui_text::kAudioParametersAdvanced), nullptr,
                                ImGuiWindowFlags_None)) return;
    ImGui::BeginChild("advancedBody", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2), false);
    ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kAdvancedOptionsHelp));
    if (monitorStarted_)
        ImGui::TextColored(ImVec4(1.00f, 0.80f, 0.30f, 1.00f), "%s",
                           wa::ui_text::get(wa::ui_text::kRunningParamsReadOnly));
    ImGui::Separator();

    ImGui::BeginDisabled(monitorStarted_);              // view-only while running

    if (ImGui::BeginTable("advancedStreams", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kAdvancedCaptureSection));
        ImGui::PushID("capP");
        ImGui::PushItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x * 0.42f));
        drawCaptureStreamParams(capParams_);
        ImGui::PopItemWidth();
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::SeparatorText(wa::ui_text::id(wa::ui_text::kAdvancedRenderSection));
        ImGui::PushID("renP");
        ImGui::PushItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x * 0.42f));
        bool renProps = renParams_.clientProperties.enabled;
        if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kAdvancedSetClientProperties), &renProps))
            renParams_.clientProperties.enabled = renProps;
        ImGui::BeginDisabled(!renParams_.clientProperties.enabled);
        int v = (int)renParams_.clientProperties.category;
        const char* categories[wa::ui_text::kAudioCategoryCount];
        fillCatalogLookup(wa::ui_text::kAudioCategoryLabels, categories, wa::ui_text::kAudioCategoryCount);
        if (ImGui::Combo(wa::ui_text::id(wa::ui_text::kCategory), &v, categories,
                         wa::ui_text::kAudioCategoryCount))
            renParams_.clientProperties.category = (wa::AudioCategory)v;
        bool off = renParams_.clientProperties.offload;
        if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kAdvancedHardwareOffload), &off))
            renParams_.clientProperties.offload = off;
        v = (int)renParams_.clientProperties.option;
        const char* streamOptions[wa::ui_text::kAdvancedStreamOptionCount];
        fillCatalogLookup(wa::ui_text::kAdvancedStreamOptions, streamOptions,
                     wa::ui_text::kAdvancedStreamOptionCount);
        if (ImGui::Combo(wa::ui_text::id(wa::ui_text::kStreamOptionCaption), &v, streamOptions,
                         wa::ui_text::kAdvancedStreamOptionCount))
            renParams_.clientProperties.option = (wa::StreamOption)v;
        ImGui::EndDisabled();
        bool duck = renParams_.ducking == wa::DuckingMode::OptOut;
        if (ImGui::Checkbox(wa::ui_text::id(wa::ui_text::kAdvancedDuckingOptOut), &duck)) renParams_.ducking = duck ? wa::DuckingMode::OptOut : wa::DuckingMode::Default;
        v = (int)renParams_.bufferMs;
        if (ImGui::InputInt(wa::ui_text::id(wa::ui_text::kBufferMs), &v))
            renParams_.bufferMs = (uint32_t)std::clamp(v, 0, 2000);
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kResetToSystemDefaults)))
            renParams_ = wa::StreamParams{};
        ImGui::PopItemWidth();
        ImGui::PopID();
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kClose), ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void AppUi::drawChartsFreezeToolbar(VisualState& viz, const wa::MonitorStatus& status) {
    // Shared by Monitor / Loopback / Application Loopback: freeze + time zoom / reset.
    const bool overallRunning =
        (status.overall == wa::StreamState::Running && status.sampleRate > 0);
    viz.chartsFrozen = wa::charts_freeze::applyLifecycle(overallRunning, viz.chartsFrozen);

    ImGui::BeginDisabled(!wa::charts_freeze::isControlEnabled(overallRunning));
    if (ImGui::Button(viz.chartsFrozen ? wa::ui_text::id(wa::ui_text::kChartsResume) : wa::ui_text::id(wa::ui_text::kChartsPause)))
        viz.chartsFrozen = !viz.chartsFrozen;
    ImGui::EndDisabled();
    if (viz.chartsFrozen) {
        ImGui::SameLine();
        ImGui::TextUnformatted(wa::ui_text::get(wa::ui_text::kChartsPaused));
    }

    // History length for the shared time axis (same window as wave/spec buffers).
    const uint32_t sr = (status.sampleRate > 0) ? status.sampleRate
                       : (viz.waveSr > 0)       ? viz.waveSr
                                                : 48000u;
    const double historyH = (viz.waveN > 0 && viz.waveSr > 0)
                                ? (double)viz.waveN / (double)viz.waveSr
                                : (double)(kSpecCols * kFftHop) / (double)sr;
    const double hopSec = (double)kFftHop / (double)sr;
    if (viz.xLink1 <= viz.xLink0)
        viz.xLink1 = historyH > 0.0 ? historyH : viz.xLink0;

    ImGui::SameLine();
    ImGui::BeginDisabled(!wa::charts_time_zoom::canZoomOut(viz.xLink0, viz.xLink1, historyH));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kChartsZoomOut))) {
        const auto z = wa::charts_time_zoom::zoomCentered(viz.xLink0, viz.xLink1, historyH, hopSec, 2.0);
        viz.xLink0 = z.x0;
        viz.xLink1 = z.x1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(
        !wa::charts_time_zoom::canZoomIn(viz.xLink0, viz.xLink1, historyH, hopSec));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kChartsZoomIn))) {
        const auto z = wa::charts_time_zoom::zoomCentered(viz.xLink0, viz.xLink1, historyH, hopSec, 0.5);
        viz.xLink0 = z.x0;
        viz.xLink1 = z.x1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kChartsResetView))) {
        const auto full = wa::charts_time_zoom::fullHistory(historyH);
        viz.xLink0 = full.x0;
        viz.xLink1 = full.x1;
        viz.resetYAxes = true; // applied once by each plot this frame; cleared after charts draw
    }
}

void AppUi::drawChartHost(wa::MonitorEngine* engine, const wa::MonitorStatus& status,
                          VisualState& viz, wa::chart_host::Mode mode, const char* caption,
                          std::vector<int>* order, wa::ScopeReader* captureReader) {
    const bool dual = mode == wa::chart_host::Mode::DualReorderable;
    ensureRunningVisuals(status, viz, dual);
    drawChartsFreezeToolbar(viz, status);
    if (caption && caption[0]) ImGui::TextUnformatted(caption);
    const uint32_t channels = std::min(status.captureChannels, 8u);
    if (channels > 1) {
        const std::string preview = viz.chartChannel < 0 ? wa::ui_text::get(wa::ui_text::kAllChannels)
            : "Ch " + std::to_string(viz.chartChannel + 1);
        ImGui::SetNextItemWidth(140.f);
        if (ImGui::BeginCombo("##chartChannel", preview.c_str())) {
            if (ImGui::Selectable(wa::ui_text::get(wa::ui_text::kAllChannels), viz.chartChannel < 0)) viz.chartChannel = -1;
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const std::string label = "Ch " + std::to_string(ch + 1);
                if (ImGui::Selectable(label.c_str(), viz.chartChannel == static_cast<int>(ch))) viz.chartChannel = static_cast<int>(ch);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Text(wa::ui_text::get(viz.probeDirection == 1 ? wa::ui_text::kRenderChannelsShown : wa::ui_text::kAvailableCaptureChannels), channels, status.captureChannels);
    }
    if (dual && engine) {
        auto ids = wa::chart_host::resolvePanelIds(mode, order ? *order : std::vector<int>{});
        const char* layouts[] = {wa::ui_text::get(wa::ui_text::kFitBoth), wa::ui_text::get(wa::ui_text::kDetailedCharts),
            wa::ui_text::get(wa::ui_text::kCaptureDevice), wa::ui_text::get(wa::ui_text::kRenderDevice)};
        ImGui::SetNextItemWidth(160.f);
        ImGui::Combo("##monitorLayout", &monitorChartLayout_, layouts, 4);
        continueWorkspaceRow(150.f);
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kSwapCharts))) {
            std::swap(ids[0], ids[1]);
            if (order) *order = ids;
        }
        continueWorkspaceRow(150.f);
        if (ImGui::Button(wa::ui_text::id(wa::ui_text::kResetLayout))) {
            monitorChartLayout_ = 0; monitorConfigFrac_ = 0.27f;
            viz.comboRatio = 0.5f; viz.chartChannel = 0;
        }
        const bool detailed = monitorChartLayout_ == 1;
        viz.fitCharts = !detailed;
        const bool both = monitorChartLayout_ < 2;
        const bool sideBySide = both && !detailed && ImGui::GetContentRegionAvail().x >= 720.f;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float paneW = sideBySide ? (ImGui::GetContentRegionAvail().x - gap) * 0.5f : 0.f;
        const float paneH = detailed ? 0.f : both && !sideBySide
            ? std::max(180.f, (ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y) * 0.5f) : 0.f;
        for (int pos = 0; pos < 2; ++pos) {
            const int id = ids[static_cast<size_t>(pos)];
            if (!both && id != monitorChartLayout_ - 2) continue;
            if (sideBySide && pos) ImGui::SameLine();
            ImGui::PushID(id);
            if (!detailed) ImGui::BeginChild("streamCharts", ImVec2(paneW, paneH), true);
            ImGui::TextWrapped("%s", chartTitle(id));
            if (id == 1) ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kRenderDownmix));
            else if (status.sampleRate && status.captureChannels) ImGui::TextDisabled("%u Hz / %u ch", status.sampleRate, status.captureChannels);
            else ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kUnavailableFormat));
            drawMonitorDumpControls(id ? status.renderDump : status.capDump,
                monitorStarted_ && (id ? playbackEnabled_ && status.renderState == wa::StreamState::Running
                    : status.capState == wa::StreamState::Running),
                id ? DumpPickKind::MonitorRen : DumpPickKind::MonitorCap,
                id ? wa::ui_text::kDumpRender : wa::ui_text::kDumpCapture);
            ImGui::ProgressBar(id ? status.renderLevel : status.capLevel, ImVec2(-1, 6.f), "");
            wa::MonitorScopeReader cap(*engine, wa::MonitorScopeReader::Side::Capture);
            wa::MonitorScopeReader ren(*engine, wa::MonitorScopeReader::Side::Render);
            drawChartPanel(id, cap, &ren, status, viz);
            if (!detailed) ImGui::EndChild();
            ImGui::PopID();
        }
    } else if (captureReader) {
        drawChartPanel(0, *captureReader, nullptr, status, viz);
    } else if (engine) {
        wa::MonitorScopeReader cap(*engine, wa::MonitorScopeReader::Side::Capture);
        drawChartPanel(0, cap, nullptr, status, viz);
    }
    viz.resetYAxes = false;
}

// Plasma-like colormap whose low end fades to fully transparent: silence shows the normal plot
// background + grid (like the other charts) instead of a solid dark-blue field; energy fades in
// through purple -> red -> orange -> yellow. Registered once, lazily.
static ImPlotColormap waSpectroColormap() {
    static ImPlotColormap cm = -1;
    if (cm == -1) {
        static const ImVec4 c[] = {
            {0.050f, 0.030f, 0.528f, 0.00f},   // floor: fully transparent
            {0.294f, 0.012f, 0.631f, 0.55f},
            {0.492f, 0.012f, 0.658f, 0.90f},
            {0.658f, 0.135f, 0.588f, 1.00f},
            {0.798f, 0.280f, 0.470f, 1.00f},
            {0.902f, 0.425f, 0.353f, 1.00f},
            {0.973f, 0.586f, 0.252f, 1.00f},
            {0.940f, 0.975f, 0.131f, 1.00f},
        };
        cm = ImPlot::AddColormap("WaSpectrogram", c, 8, false);   // continuous
    }
    return cm;
}

// Draw one scrolling log-frequency spectrogram. Y is plotted in log10(Hz): the Spectrogram rows
// are log-spaced in frequency, so log10(f) is uniform in row index -> PlotHeatmap's uniform grid
// maps 1:1 onto a linear axis of log10(Hz), keeping the content correctly placed. Custom ticks
// label the real frequencies, so the axis reads in Hz. The range is set once (not Always) so the
// user can pan/zoom the frequency axis; the explicit Once limits apply on the plot's first frame,
// so the always-created (possibly empty) plot never pins to the [0,1] defaults.
void AppUi::drawSpectrogramPanel(VisualState& viz, const char* plotId, wa::SpectrogramVisual* visual,
                                 double histSec, float height, int slot) {
    const wa::Spectrogram* spec = visual ? visual->get() : nullptr;
    // spec == null (not running / no data yet): draw the axes only with a default 20..24 kHz range
    // so the panel is ALWAYS visible, consistent with the waveform panel. The explicit
    // log-range SetupAxisLimits keeps the empty plot from pinning to [0,1] (the old auto-fit bug).
    const double fmin = spec ? spec->fmin() : 20.0;
    const double fmax = spec ? spec->fmax() : 24000.0;
    const double loL  = std::log10(fmin);
    const double hiL  = std::log10(fmax);
    const double lo0  = std::log10(std::max(fmin, 100.0));   // initial view floor = smallest labeled tick (100 Hz)
    // Audition-style log-frequency ruler (1-2-3-4-6 series). Ticks below 100 exist only for
    // zoom-out; the initial view starts at 100 so the smallest visible tick is 100.
    static const double kFreq[]  = {20, 30, 40, 60, 100, 200, 300, 400, 600, 1000, 2000, 3000, 4000, 6000, 10000, 20000};
    static const char*  kFreqL[] = {"20", "30", "40", "60", "100", "200", "300", "400", "600", "1k", "2k", "3k", "4k", "6k", "10k", "20k"};
    constexpr int kNFreq = 16;
    double tickV[kNFreq]; const char* tickL[kNFreq]; int nTick = 0;
    for (int i = 0; i < kNFreq; ++i)
        if (kFreq[i] >= fmin && kFreq[i] <= fmax) {
            const double minimumLogGap = (hiL - lo0) * ImGui::GetTextLineHeightWithSpacing() /
                std::max(40.f, height - ImGui::GetTextLineHeight() * 3.f);
            if (nTick && std::log10(kFreq[i]) - tickV[nTick - 1] < minimumLogGap) continue;
            tickV[nTick] = std::log10(kFreq[i]); tickL[nTick] = kFreqL[i]; ++nTick;
        }
    ImPlot::PushColormap(waSpectroColormap());   // Plasma-like, transparent at silence (grid shows)
    if (ImPlot::BeginPlot(plotId, ImVec2(-1, height))) {
        const int channel = slot == kSlotRenSpectro ? 0 : slot - kSlotCapSpectroBase;
        frameProbe_.addWork("visible-spec-plots", 1, viz.probeSource, viz.probeDrawDirection, channel);
        // Y locked while the plot area was hovered last frame -> in-plot wheel/drag act on X only;
        // hover the Y ruler (plot-area hover = false -> unlocked) to zoom/pan the frequency axis.
        // Audition-style: no time labels here (the waveform above carries the top time ruler);
        // Hz ruler on the RIGHT.
        const ImPlotAxisFlags yf = (viz.plotHovPrev[slot] ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None)
                                 | ImPlotAxisFlags_Opposite;
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, yf);
        ImPlot::SetupAxisLinks(ImAxis_X1, &viz.xLink0, &viz.xLink1);   // shared time axis (waveforms + spectrograms)
        // Clamp panning/zooming to the buffered history / frequency range — no empty space.
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, histSec);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, loL, hiL);
        ImPlot::SetupAxisLimits(ImAxis_Y1, lo0, hiL,
                                viz.resetYAxes ? ImGuiCond_Always : ImGuiCond_Once);
        if (nTick > 0) ImPlot::SetupAxisTicks(ImAxis_Y1, tickV, nTick, tickL);
        if (spec) {
            wa::FrameBracket heatmap(frameProbe_, wa::FrameSlice::Heatmap);
            // Texture path: one PlotImage quad instead of rows*cols ImPlot rects.
            // HeatmapTexture::ensure skips RGBA rebuild + upload when revision is unchanged.
            static uint32_t lut[256];
            static bool lutReady = false;
            if (!lutReady) {
                static const wa::HeatmapColorStop kStops[] = {
                    {0.050f, 0.030f, 0.528f, 0.00f},
                    {0.294f, 0.012f, 0.631f, 0.55f},
                    {0.492f, 0.012f, 0.658f, 0.90f},
                    {0.658f, 0.135f, 0.588f, 1.00f},
                    {0.798f, 0.280f, 0.470f, 1.00f},
                    {0.902f, 0.425f, 0.353f, 1.00f},
                    {0.973f, 0.586f, 0.252f, 1.00f},
                    {0.940f, 0.975f, 0.131f, 1.00f},
                };
                wa::buildHeatmapLut(kStops, 8, lut);
                lutReady = true;
            }
            bool drew = false;
            if (auto* texture = visual->ensure(lut, frameProbe_.enabled() ? &frameProbe_ : nullptr,
                                               viz.probeSource, viz.probeDrawDirection, channel)) {
                ImPlot::PlotImage("##hm", texture->textureId(),
                                  ImPlotPoint(0, loL), ImPlotPoint(histSec, hiL));
                drew = true;
            }
            if (!drew) {
                frameProbe_.addWork("heatmap-fallback", 1, viz.probeSource, viz.probeDrawDirection, channel);
                ImPlot::PlotHeatmap("##hm", spec->data(), spec->rows(), spec->cols(), -96.0, 0.0,
                                    nullptr, ImPlotPoint(0, loL), ImPlotPoint(histSec, hiL));
            }
        }
        drawYUnitLabel("Hz", true);
        viz.plotHovPrev[slot] = ImPlot::IsPlotHovered();
        ImPlot::EndPlot();
    }
    ImPlot::PopColormap();
}

// Draw one waveform over the shared time axis (xLink0_..xLink1_ seconds). wave[i] is at t = i/sr,
// so the buffer's tail holds the newest samples (right edge). Level-of-detail: zoomed in -> raw
// line; zoomed out -> per-pixel min/max envelope with translucent fill + opaque outline so far
// view stays readable while remaining column-aligned with the spectrogram. Optional MinMaxPyramid
// updates overwritten blocks and reuses the index while frozen / panning / zooming.
void AppUi::drawWaveformPanel(VisualState& viz, const char* plotId, wa::WaveHistoryView wave,
                              uint32_t sr, bool haveData, float height, int slot,
                              wa::MinMaxPyramid* pyr) {
    const int n = wave.n;
    if (!ImPlot::BeginPlot(plotId, ImVec2(-1, height))) return;
    const int channel = slot == kSlotRenWave ? 0 : slot - kSlotCapWaveBase;
    const auto note = [&](const char* metric, uint64_t value) {
        frameProbe_.addWork(metric, value, viz.probeSource, viz.probeDrawDirection, channel);
    };
    note("visible-wave-plots", 1);
    // Y locked while the plot area was hovered last frame -> in-plot wheel/drag act on X only;
    // zoom amplitude via the Y ruler. X tick labels hidden: the spectrogram below shows the time.
    // Audition-style: time ruler on TOP of the waveform, dB ruler on the RIGHT.
    const ImPlotAxisFlags yf = (viz.plotHovPrev[slot] ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None)
                             | ImPlotAxisFlags_Opposite;
    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_Opposite, yf);
    ImPlot::SetupAxisLinks(ImAxis_X1, &viz.xLink0, &viz.xLink1);        // shared time axis
    // Clamp panning/zooming to the buffered history — no scrolling into empty space.
    const double hist = (sr > 0 && n > 0) ? (double)n / (double)sr
                                          : (double)(kSpecCols * kFftHop) / 48000.0;
    ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, hist);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, -1.0, 1.0);
    ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0,
                            viz.resetYAxes ? ImGuiCond_Always : ImGuiCond_Once);
    // dB ruler for the warped scale (positions = dbWarp of the label, with kWaveDbFloor = -60).
    static const double kAmpV[] = {-1.0, -0.9, -0.8, -0.6, -0.2, 0.0, 0.2, 0.6, 0.8, 0.9, 1.0};
    static const char*  kAmpL[] = {"0", "-6", "-12", "-24", "-48", "-inf", "-48", "-24", "-12", "-6", "0"};
    if (height < ImGui::GetTextLineHeight() * 6.f) {
        static const double centerV[] = {0.};
        static const char* centerL[] = {"-inf"};
        ImPlot::SetupAxisTicks(ImAxis_Y1, centerV, 1, centerL);
    } else if (height < ImGui::GetTextLineHeight() * 15.f) {
        static const double compactV[] = {-1., 0., 1.};
        static const char* compactL[] = {"0", "-inf", "0"};
        ImPlot::SetupAxisTicks(ImAxis_Y1, compactV, 3, compactL);
    } else ImPlot::SetupAxisTicks(ImAxis_Y1, kAmpV, 11, kAmpL);
    if (haveData && n > 0 && sr > 0 && wave.data) {
        const double invSr = 1.0 / (double)sr;
        int iLo = (int)std::floor(viz.xLink0 * (double)sr);
        int iHi = (int)std::ceil (viz.xLink1 * (double)sr);
        if (iLo < 0) iLo = 0;
        if (iHi > n - 1) iHi = n - 1;
        if (iHi > iLo) {
            const int visN = iHi - iLo + 1;
            float pw = ImPlot::GetPlotSize().x;
            if (pw < 1.0f) pw = 1.0f;
            if (visN <= (int)(2.0f * pw)) {
                note("wave-raw-samples", static_cast<uint64_t>(visN));
                viz.envMax.resize((size_t)visN);                     // scratch: warped raw samples
                for (int i = 0; i < visN; ++i) viz.envMax[(size_t)i] = dbWarp(wave[iLo + i]);
                ImPlot::SetNextLineStyle(kWaveColor);
                ImPlot::PlotLine("##wave", viz.envMax.data(), visN, invSr, (double)iLo * invSr);
            } else {
                const int cols = (int)pw;
                viz.envX.resize((size_t)cols); viz.envMin.resize((size_t)cols); viz.envMax.resize((size_t)cols);
                wa::WaveEnvelopeWork work;
                {
                    // Resize stays outside: it can allocate. The bracket times the min/max fill only.
                    wa::FrameBracket envelope(frameProbe_, wa::FrameSlice::Envelope);
                    const auto& state = slot == kSlotRenWave ? viz.renderWaveState : viz.capWaveState;
                    wa::fillWaveEnvelope(wave, iLo, iHi, cols, invSr, pyr, state.revision,
                                         viz.envX.data(), viz.envMin.data(), viz.envMax.data(),
                                         frameProbe_.enabled() ? &work : nullptr, state.generation);
                }
                note("wave-block-rebuild-samples", work.rebuildSamples);
                note("wave-block-update-samples", work.updateSamples);
                note("wave-exact-samples", work.exactSamples);
                note("wave-block-queries", work.blockQueries);
                note("wave-envelope-columns", work.columns);
                // Translucent body (FillAlpha multiplies color.w) + opaque min/max outline.
                ImPlot::SetNextFillStyle(kWaveColor, kWaveEnvFillAlpha);
                ImPlot::PlotShaded("##wave", viz.envX.data(), viz.envMin.data(), viz.envMax.data(), cols);
                ImPlot::SetNextLineStyle(kWaveColor, kWaveEnvLineWeight);
                ImPlot::PlotLine("##waveMax", viz.envX.data(), viz.envMax.data(), cols);
                ImPlot::SetNextLineStyle(kWaveColor, kWaveEnvLineWeight);
                ImPlot::PlotLine("##waveMin", viz.envX.data(), viz.envMin.data(), cols);
            }
        }
    }
    drawYUnitLabel("dB", true);
    viz.plotHovPrev[slot] = ImPlot::IsPlotHovered();
    ImPlot::EndPlot();
}

// One combined cell per stream: waveform on top, spectrogram below (Audition-style), sharing the
// linked time axis, with a draggable horizontal splitter between them. comboRatio_ is shared by
// both streams so the capture and render cells stay height-aligned for side-by-side comparison.
void AppUi::drawComboPanel(wa::ScopeReader& reader, const wa::MonitorStatus& status,
                           VisualState& viz, bool renderSide) {
    const float comboH = viz.fitCharts ? std::max(120.f, ImGui::GetContentRegionAvail().y - kSplitH - ImGui::GetStyle().ItemSpacing.y * 2.f) : kComboH;
    const uint32_t sr         = status.sampleRate;
    const bool overallRunning = (status.overall     == wa::StreamState::Running && sr > 0);
    const bool renderRunning  = (status.renderState == wa::StreamState::Running);
    const auto capturePlan = wa::capture_channel_view::makePlan(status.captureChannels);
    // Split layout follows the capture plan whenever the page is running (including frozen),
    // so multi-channel panels stay mounted while chart data is held.
    const bool splitCapture = !renderSide && overallRunning && capturePlan.split;

    // Chart Data Pipeline: hop-aligned spectrum + latest wave history; freeze gated inside.
    wa::ChartRefreshParams refreshParams;
    viz.probeDrawDirection = renderSide ? 1 : viz.probeDirection;
    refreshParams.probe = frameProbe_.enabled() ? &frameProbe_ : nullptr;
    refreshParams.source = viz.probeSource;
    refreshParams.direction = viz.probeDrawDirection;
    refreshParams.reader = &reader;
    refreshParams.frozen = viz.chartsFrozen;
    // Live: require overall (+ render when render-side). Frozen: still "active" so haveWave
    // can report retained buffers; pipeline skips writes when frozen.
    refreshParams.streamActive =
        overallRunning && (viz.chartsFrozen || !renderSide || renderRunning);
    refreshParams.fftWin = kFftWin;
    refreshParams.fftHop = kFftHop;
    refreshParams.maxCatchup = kCatchup;

    if (splitCapture) {
        const uint32_t actualChannels = capturePlan.actualChannels;
        const uint32_t shownChannels = std::min<uint32_t>(
            capturePlan.visibleChannels, (uint32_t)viz.capChannelWaves.size());
        if (actualChannels > shownChannels)
            ImGui::Text(wa::ui_text::get(viz.probeDirection == 1 ? wa::ui_text::kRenderChannelsShown : wa::ui_text::kAvailableCaptureChannels),
                        shownChannels, actualChannels);

        wa::ChartBuffers buffers;
        buffers.channelWaves = &viz.capChannelWaves;
        buffers.channelCount = shownChannels;
        buffers.waveN = viz.waveN;
        buffers.waveSr = viz.waveSr;
        buffers.waveState = &viz.capWaveState;
        buffers.waveScratch = &waveSnapshotScratch_;
        buffers.channelSpecWindows = &viz.capSpecWindows;
        std::array<wa::Spectrogram*, 8> spectra{};
        const uint32_t specCount = std::min<uint32_t>(shownChannels, static_cast<uint32_t>(viz.capChannelSpecs.size()));
        for (uint32_t ch = 0; ch < specCount; ++ch) spectra[ch] = viz.capChannelSpecs[ch]->get();
        buffers.channelSpecs = spectra.data();
        buffers.channelSpecCount = specCount;
        buffers.work = &viz.workCap;
        buffers.mag = &viz.magCap;
        buffers.nextEnd = &viz.nextCapEnd;
        buffers.specSr = viz.specSr;
        const wa::ChartRefreshResult refreshed = wa::refreshCharts(refreshParams, buffers);

        if (viz.capEnvPyramids.size() < (size_t)shownChannels)
            viz.capEnvPyramids.resize((size_t)shownChannels);

#ifdef WA_PERFORMANCE_FIXTURE
        PerformanceFixture::chartPosition(viz.probeSource, false);
#endif
        const uint32_t first = viz.chartChannel < 0 ? 0u : std::min(static_cast<uint32_t>(viz.chartChannel), shownChannels ? shownChannels - 1 : 0u);
        const uint32_t last = viz.chartChannel < 0 ? shownChannels : std::min(first + 1, shownChannels);
        const float waveHeight = viz.fitCharts && viz.chartChannel >= 0 ? std::max(65.f, comboH * viz.comboRatio - ImGui::GetTextLineHeightWithSpacing()) : kMultiWaveH;
        const float specHeight = viz.fitCharts && viz.chartChannel >= 0 ? std::max(65.f, comboH * (1.f - viz.comboRatio) - ImGui::GetTextLineHeightWithSpacing()) : kMultiSpecH;
        for (uint32_t ch = first; ch < last; ++ch) {
            std::vector<float>& wave = viz.capChannelWaves[(size_t)ch];
            char title[64];
            std::snprintf(title, sizeof(title),
                          wa::ui_text::get(viz.probeDirection == 1 ? wa::ui_text::kRenderChannelWaveform : wa::ui_text::kCaptureChannelWaveform), ch + 1u);
            ImGui::TextUnformatted(title);
            // Skip envelope work for channels scrolled out of the chart host clip rect.
            const float waveW = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
            const ImVec2 wavePos = ImGui::GetCursorScreenPos();
            const ImVec2 waveMax(wavePos.x + waveW, wavePos.y + waveHeight);
            if (!ImGui::IsRectVisible(wavePos, waveMax)) {
                ImGui::Dummy(ImVec2(waveW, waveHeight));
                viz.plotHovPrev[kSlotCapWaveBase + (int)ch] = false;
                continue;
            }
            const std::string plotId = "##capWaveCh" + std::to_string(ch);
            drawWaveformPanel(viz, plotId.c_str(), viz.capWaveState.view(wave.data()), viz.waveSr,
                              refreshed.haveWave, waveHeight, kSlotCapWaveBase + (int)ch,
                              &viz.capEnvPyramids[(size_t)ch]);
        }

        const uint32_t hz = (sr > 0) ? sr : 48000u;
        const double histSec = (double)(kSpecCols * kFftHop) / (double)hz;
#ifdef WA_PERFORMANCE_FIXTURE
        PerformanceFixture::chartPosition(viz.probeSource, true);
#endif
        for (uint32_t ch = first; ch < last; ++ch) {
            char title[64];
            std::snprintf(title, sizeof(title),
                          wa::ui_text::get(viz.probeDirection == 1 ? wa::ui_text::kRenderChannelSpectrogram : wa::ui_text::kCaptureChannelSpectrogram), ch + 1u);
            ImGui::TextUnformatted(title);
            const std::string plotId = "##capSpecCh" + std::to_string(ch);
            wa::SpectrogramVisual* spec = (ch < viz.capChannelSpecs.size())
                                        ? viz.capChannelSpecs[(size_t)ch].get()
                                        : nullptr;
            drawSpectrogramPanel(viz, plotId.c_str(), spec, histSec, specHeight,
                                 kSlotCapSpectroBase + (int)ch);
        }
        return;
    }

    std::vector<float>& wave = renderSide ? viz.renderWave : viz.capWave;
    wa::ChartBuffers buffers;
    buffers.wave = wave.empty() ? nullptr : wave.data();
    buffers.waveN = viz.waveN;
    buffers.waveSr = viz.waveSr;
    buffers.waveState = renderSide ? &viz.renderWaveState : &viz.capWaveState;
    buffers.waveScratch = &waveSnapshotScratch_;
    buffers.specWin = &viz.specWin;
    buffers.work = renderSide ? &viz.workRender : &viz.workCap;
    buffers.mag = renderSide ? &viz.magRender : &viz.magCap;
    buffers.spectrogram = renderSide ? viz.renderSpec.get() : viz.capSpec.get();
    buffers.nextEnd = renderSide ? &viz.nextRenderEnd : &viz.nextCapEnd;
    buffers.specSr = viz.specSr;
    const wa::ChartRefreshResult refreshed = wa::refreshCharts(refreshParams, buffers);
    bool ok = refreshed.haveWave;

    const float waveH = comboH * viz.comboRatio;
    drawWaveformPanel(viz, renderSide ? "##renWave" : "##capWave", buffers.waveState->view(wave.data()), viz.waveSr, ok,
                      waveH, renderSide ? kSlotRenWave : kSlotCapWaveBase, renderSide ? &viz.renderEnvPyramid : &viz.capEnvPyramid);

    // Splitter: drag to rebalance waveform vs spectrogram height (the axes re-lay out to fit).
    ImGui::InvisibleButton(renderSide ? "##renSplit" : "##capSplit",
                           ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), kSplitH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive())
        viz.comboRatio = std::clamp(viz.comboRatio + ImGui::GetIO().MouseDelta.y / comboH, 0.15f, 0.85f);
    const ImVec2 smn = ImGui::GetItemRectMin(), smx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(smn.x, (smn.y + smx.y) * 0.5f),
                                        ImVec2(smx.x, (smn.y + smx.y) * 0.5f),
                                        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);

    wa::SpectrogramVisual* spec = nullptr;
    if (overallRunning) {
        // Live: render spectrogram only while playback runs. Frozen: keep last renderSpec even
        // if playback was turned off mid-freeze (resetRenderVisuals is suppressed).
        if (renderSide)
            spec = (renderRunning || viz.chartsFrozen) ? &viz.renderSpec : nullptr;
        else
            spec = &viz.capSpec;
    }
    const uint32_t hz = (sr > 0) ? sr : 48000u;   // idle: assume 48 kHz for the axis ranges
    drawSpectrogramPanel(viz, renderSide ? "##renSpec" : "##capSpec", spec,
                         (double)(kSpecCols * kFftHop) / (double)hz, comboH - waveH,
                         renderSide ? kSlotRenSpectro : kSlotCapSpectroBase);
}

void AppUi::drawChartPanel(int id, wa::ScopeReader& captureReader, wa::ScopeReader* renderReader,
                           const wa::MonitorStatus& status, VisualState& viz) {
    switch (id) {
    case 0:
        drawComboPanel(captureReader, status, viz, false);
        break;
    case 1:
        if (renderReader) drawComboPanel(*renderReader, status, viz, true);
        break;
    default:
        break;
    }
}

void AppUi::drawCapsModal() {
    prepareAudioModal(860, 600);
    if (!ImGui::BeginPopupModal(wa::ui_text::id(wa::ui_text::kDeviceCapabilities), nullptr,
                                ImGuiWindowFlags_None)) return;

    ImGui::BeginChild("capsBody", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y * 2), false);
    if (capsModalContext_)
        ImGui::TextUnformatted(wa::ui_text::get(capsModalContext_));

    ImGui::TextWrapped("%s", capsDeviceSummary_.c_str());
    ImGui::TextDisabled("%s", wa::ui_text::get(wa::ui_text::kCapabilitySource));
    if (ImGui::BeginTable("sources", 3, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kOriginMix));
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kOriginDevice));
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kOriginOem));
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (capsModal_.hasMix) {
            ImGui::TextWrapped("%s", formatLabel(capsModal_.mixFormat).c_str());
            ImGui::TextWrapped("%s", wa::channelLayoutLabel(capsModal_.mixFormat).c_str());
        } else {
            ImGui::TextUnformatted("-");
        }
        ImGui::TableSetColumnIndex(1);
        if (capsModal_.hasDevice) {
            ImGui::TextWrapped("%s", formatLabel(capsModal_.deviceFormat).c_str());
            ImGui::TextWrapped("%s", wa::channelLayoutLabel(capsModal_.deviceFormat).c_str());
        } else {
            ImGui::TextUnformatted("-");
        }
        ImGui::TableSetColumnIndex(2);
        if (capsModal_.hasOem) {
            ImGui::TextWrapped("%s", formatLabel(capsModal_.oemFormat).c_str());
            ImGui::TextWrapped("%s", wa::channelLayoutLabel(capsModal_.oemFormat).c_str());
        } else {
            ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();

    ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x * .48f));
    ImGui::InputTextWithHint("##capsFilter", wa::ui_text::get(wa::ui_text::kCapabilitiesFilter), capsFilter_, sizeof(capsFilter_));
    ImGui::SameLine();
    const char* supportChoices[] = {wa::ui_text::get(wa::ui_text::kAnySupport),
        wa::ui_text::get(wa::ui_text::kSharedExact), wa::ui_text::get(wa::ui_text::kExclusiveExact)};
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##capsSupport", &capsSupportFilter_, supportChoices, 3);
    const auto capsMatch = [&](const wa::FormatSupport& fs) {
        const std::string text = formatLabel(fs.fmt) + " " + wa::channelLayoutLabel(fs.fmt) + " " + wa::ui_text::formatOriginsLabel(fs.origins);
        return wa::call_log_filter::fuzzyMatch(text, capsFilter_) &&
            (capsSupportFilter_ == 0 || (capsSupportFilter_ == 1 ? fs.shared : fs.exclusive) == wa::SupportLevel::Exact);
    };
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kCopyVisible))) {
        std::string text = capsDeviceSummary_ + "\nFormat\tLayout\tSources\tShared\tExclusive\n";
        for (const auto& fs : capsModal_.matrix) if (capsMatch(fs))
            text += formatLabel(fs.fmt) + "\t" + wa::channelLayoutLabel(fs.fmt) + "\t" + wa::ui_text::formatOriginsLabel(fs.origins)
                + "\t" + wa::ui_text::supportLabel(fs.shared) + "\t" + wa::ui_text::supportLabel(fs.exclusive) + "\n";
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(capsSelected_ < 0 || (capsModalContext_ == wa::ui_text::kMonitorCaptureDevice && monitorStarted_));
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kUseRequestFormat)) && capsSelected_ >= 0) {
        const auto& fmt = capsModal_.matrix[static_cast<size_t>(capsSelected_)].fmt;
        if (capsModalContext_ == wa::ui_text::kMonitorCaptureDevice) {
            selectedFmt_ = fmt; haveFmt_ = true; fmtChoiceIdx_ = -1;
        } else if (capsModalContext_ == wa::ui_text::kSystemLoopbackRenderDevice)
            wa::create_recipe::selectCandidate(loopbackRecipe_.format, fmt, -1);
        else if (capsModalContext_ == wa::ui_text::kApplicationLoopbackFormatReference)
            wa::create_recipe::selectCandidate(appLoopbackRecipe_.format, fmt, -1);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", wa::ui_text::get(wa::ui_text::kRequestFormatHelp));
    if (capsSelected_ >= 0)
        ImGui::Text("%s: %s", wa::ui_text::get(wa::ui_text::kRequestedFormat),
            formatLabel(capsModal_.matrix[static_cast<size_t>(capsSelected_)].fmt).c_str());
    ImGui::TextWrapped("%s", wa::ui_text::get(wa::ui_text::kClosestHelp));
    if (ImGui::BeginTable("caps", 5,
                          ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kFormat));
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kRenderLayout));
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kSources));
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kShared));
        ImGui::TableSetupColumn(wa::ui_text::id(wa::ui_text::kExclusive));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < capsModal_.matrix.size(); ++i) {
            const auto& fs = capsModal_.matrix[i];
            if (!capsMatch(fs)) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(formatLabel(fs.fmt).c_str(), capsSelected_ == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns))
                capsSelected_ = static_cast<int>(i);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(wa::channelLayoutLabel(fs.fmt).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(wa::ui_text::formatOriginsLabel(fs.origins).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(wa::ui_text::supportLabel(fs.shared));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(wa::ui_text::supportLabel(fs.exclusive));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button(wa::ui_text::id(wa::ui_text::kClose), ImVec2(120.0f, 0.0f)))
        ImGui::CloseCurrentPopup();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
