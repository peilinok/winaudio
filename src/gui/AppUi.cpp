#include "AppUi.h"
#include "ApplicationLoopbackUiModel.h"
#include "AppUiText.h"
#include "CaptureChannelView.h"
#include "ChartDataPipeline.h"
#include "ChartsFreezePolicy.h"
#include "ChartsTimeZoomPolicy.h"
#include "ComUtil.h"
#include "DumpUi.h"
#include "MonitorScopeReader.h"
#include "imgui.h"
#include "implot.h"
#include "FormatSpec.h"
#include "Log.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

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
    const float ax = std::fabs(x);
    if (ax <= 1e-6f) return 0.f;
    const float db = 20.0f * std::log10(ax);
    if (db <= kWaveDbFloor) return 0.f;
    const float p = 1.0f - db / kWaveDbFloor;   // 0 dB -> 1, floor -> 0
    return (x < 0.f) ? -p : p;
}
} // namespace

// Draw a 4-way "move" icon (crosshair + outward arrowheads) centered at c, sized to a box of side s.
static void drawMoveIcon(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const float tip = s * 0.42f, aw = s * 0.12f, ah = s * 0.16f, th = 1.5f;
    dl->AddLine(ImVec2(c.x - tip, c.y), ImVec2(c.x + tip, c.y), col, th);   // horizontal arm
    dl->AddLine(ImVec2(c.x, c.y - tip), ImVec2(c.x, c.y + tip), col, th);   // vertical arm
    dl->AddTriangleFilled(ImVec2(c.x, c.y - tip), ImVec2(c.x - aw, c.y - tip + ah), ImVec2(c.x + aw, c.y - tip + ah), col); // up
    dl->AddTriangleFilled(ImVec2(c.x, c.y + tip), ImVec2(c.x - aw, c.y + tip - ah), ImVec2(c.x + aw, c.y + tip - ah), col); // down
    dl->AddTriangleFilled(ImVec2(c.x - tip, c.y), ImVec2(c.x - tip + ah, c.y - aw), ImVec2(c.x - tip + ah, c.y + aw), col); // left
    dl->AddTriangleFilled(ImVec2(c.x + tip, c.y), ImVec2(c.x + tip - ah, c.y - aw), ImVec2(c.x + tip - ah, c.y + aw), col); // right
}

void AppUi::refreshMonitorDevices() {
    wa::ComInitGuard com;   // REQUIRED: GUI thread has no COM; DeviceEnumerator needs it
    const wa::DeviceId prevCap = (capDevIdx_ >= 0 && capDevIdx_ < (int)capDevices_.size())
                                     ? capDevices_[(size_t)capDevIdx_].id : L"";
    const wa::DeviceId prevRen = (renderDevIdx_ >= 0 && renderDevIdx_ < (int)renderDevices_.size())
                                     ? renderDevices_[(size_t)renderDevIdx_].id : L"";
    const wa::DeviceId prevLoopback = (loopbackDevIdx_ >= 0 && loopbackDevIdx_ < (int)renderDevices_.size())
                                      ? renderDevices_[(size_t)loopbackDevIdx_].id : L"";
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
    loopbackRecipe_.deviceShown = -1;
    monitorDevicesLoaded_ = true;
}

void AppUi::stopAll() {
    monitor_.stop();
    loopbackTracks_.destroyAll();
    appLoopbackTracks_.destroyAll();
    pipelineEtw_.stop();
    pipelineAttach_.stop();
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
    wa::LiveSessionView prev;
    if (pipelineSelected_ >= 0 && pipelineSelected_ < (int)pipelineSessions_.size())
        prev = pipelineSessions_[(size_t)pipelineSelected_];

    std::vector<wa::LiveSessionView> rows;
    wa::Result r = liveSessionEnumerator_.enumerate(rows);
    pipelineSessionsLoaded_ = true;
    if (!r) {
        pipelineSessions_.clear();
        pipelineSelected_ = -1;
        pipelineNodes_.clear();
        logLines_.push_back("pipeline refresh failed: " + r.message);
        return;
    }

    const uint32_t hidePid = pipelineShowSelf_ ? 0u : static_cast<uint32_t>(GetCurrentProcessId());
    wa::shapeLiveSessionList(rows, hidePid);
    pipelineSessions_ = std::move(rows);

    pipelineSelected_ = -1;
    for (int i = 0; i < (int)pipelineSessions_.size(); ++i) {
        const auto& s = pipelineSessions_[(size_t)i];
        if (s.processId == prev.processId && s.deviceId == prev.deviceId && s.flow == prev.flow) {
            pipelineSelected_ = i;
            break;
        }
    }
    if (pipelineSelected_ < 0)
        pipelineProbes_.clear();
    rebuildPipelineGraph();
}

void AppUi::rebuildPipelineGraph() {
    pipelineNodes_.clear();
    if (pipelineSelected_ < 0 || pipelineSelected_ >= (int)pipelineSessions_.size())
        return;

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

void AppUi::applyPipelineJoin() {
    pipelineNodes_.clear();
    if (pipelineSelected_ < 0 || pipelineSelected_ >= (int)pipelineSessions_.size())
        return;
    const wa::LiveSessionView& session = pipelineSessions_[(size_t)pipelineSelected_];
    wa::EtwInitializeHint etw;
    if (pipelineEtw_.status() == wa::EtwWatchStatus::Listening)
        etw = wa::matchEtwInitialize(session, pipelineEtw_.snapshot(), wa::etwNowMs());
    pipelineEtwHint_ = etw;
    const auto hooked = wa::shapeCallLog(pipelineCalls_, false);
    pipelineNodes_ = wa::assemblePipeline(session, pipelineEndpoint_, etw, pipelineProbes_,
                                          hooked.entries);
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

void AppUi::runPipelineAttach() {
    if (pipelineSelected_ < 0 || pipelineSelected_ >= (int)pipelineSessions_.size())
        return;
    const uint32_t pid = pipelineSessions_[(size_t)pipelineSelected_].processId;
    pipelineCalls_.clear();
    wa::Result r = pipelineAttach_.start(pid);
    if (!r) {
        pipelineAttachBanner_ = r.message;
        logLines_.push_back("pipeline attach failed: " + r.message);
        applyPipelineJoin();
        return;
    }
    pipelineAttachBanner_.clear();
    pipelineAttach_.setPumpEnabled(pipelinePump_);
    logLines_.push_back("pipeline attached pid=" + std::to_string(pid));
    applyPipelineJoin();
}

void AppUi::pushLog(int /*level*/, const std::string& line) {
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
            if (fs.fmt == fmt) return fs.exclusiveOk;
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
    ImGui::Text("Format: %u/%u/%u%s%s",
                f.sampleRate, (unsigned)f.bitsPerSample, (unsigned)f.channels,
                f.isFloat ? "f" : "",
                haveFmt_ ? "" : " (default)");

    // Build filtered candidate list for the current backend mode
    const bool isExclusive = (backendIdx_ == 1);
    std::vector<wa::AudioFormat> okFmts;
    for (const auto& fs : capsCache_.matrix)
        if (isExclusive ? fs.exclusiveOk : fs.sharedOk)
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
    const std::string preview = (fmtChoiceIdx_ == 0) ? std::string("System default")
                              : (fmtChoiceIdx_ >= 1) ? fmtStr(okFmts[(size_t)(fmtChoiceIdx_ - 1)])
                              : fmtStr(selectedFmt_);   // custom (idx == -1)
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##fmtCombo", preview.c_str())) {
        // Item 0: System default — recomputes selectedFmt_ and haveFmt_ from device/backend
        if (ImGui::Selectable("System default##0", fmtChoiceIdx_ == 0)) {
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
    if (ImGui::Button(wa::ui_text::kApply)) {
        wa::AudioFormat parsed{};
        if (wa::parseFormatSpec(std::string(fmtCustom_), parsed)) {
            selectedFmt_  = parsed;
            haveFmt_      = true;
            fmtChoiceIdx_ = -1;   // custom, not a combo item
        } else {
            logLines_.push_back("invalid format");
        }
    }
}

static const char* kAudioCategories[] = {
    "Other", "Communications", "Media", "Movie",
    "Game chat", "Speech", "Sound effects", "Game media"
};

static void drawCaptureStreamParams(wa::StreamParams& p) {
    bool capProps = p.clientProperties.enabled;
    if (ImGui::Checkbox(wa::ui_text::kAdvancedSetClientProperties, &capProps))
        p.clientProperties.enabled = capProps;
    ImGui::BeginDisabled(!p.clientProperties.enabled);
    int v = (int)p.clientProperties.category;
    if (ImGui::Combo("Category", &v, kAudioCategories, IM_ARRAYSIZE(kAudioCategories)))
        p.clientProperties.category = (wa::AudioCategory)v;
    bool capOff = p.clientProperties.offload;
    if (ImGui::Checkbox(wa::ui_text::kAdvancedHardwareOffload, &capOff))
        p.clientProperties.offload = capOff;
    v = (int)p.clientProperties.option;
    if (ImGui::Combo("Stream option", &v, wa::ui_text::kAdvancedStreamOptions,
                     wa::ui_text::kAdvancedStreamOptionCount))
        p.clientProperties.option = (wa::StreamOption)v;
    ImGui::EndDisabled();
    v = (int)p.bufferMs;
    if (ImGui::InputInt("Buffer (ms)", &v)) p.bufferMs = (uint32_t)std::clamp(v, 0, 2000);
    if (ImGui::Button("Reset to system defaults")) p = wa::StreamParams{};
}

void AppUi::recomputeLoopbackFormat() {
    wa::ComInitGuard com;
    loopbackRecipe_.caps = wa::DeviceCapabilities{};
    wa::DeviceId id = (!renderDevices_.empty() && loopbackDevIdx_ >= 0
                       && loopbackDevIdx_ < (int)renderDevices_.size())
                          ? renderDevices_[(size_t)loopbackDevIdx_].id
                          : L"";
    enumerator_.queryCapabilities(wa::DataFlow::Render, id, loopbackRecipe_.caps);
    if (id != loopbackRecipe_.deviceId) {
        wa::create_recipe::selectDefault(loopbackRecipe_.format, loopbackRecipe_.caps.mixFormat);
        loopbackRecipe_.deviceId = id;
    }
    loopbackRecipe_.deviceShown = loopbackDevIdx_;
}

void AppUi::drawFormatRecipe(wa::create_recipe::FormatState& st,
                             const std::vector<wa::AudioFormat>& candidates,
                             const wa::AudioFormat& defaultDisplay,
                             const char* comboId, wa::StreamParams& params) {
    ImGui::PushID(comboId);
    const int nOk = (int)candidates.size();
    if (st.choiceIdx < -1 || st.choiceIdx > nOk) st.choiceIdx = 0;

    auto fmtStr = [](const wa::AudioFormat& fmt) -> std::string {
        std::string s = std::to_string(fmt.sampleRate) + "/" +
                        std::to_string(fmt.bitsPerSample) + "/" +
                        std::to_string(fmt.channels);
        if (fmt.isFloat) s += "f";
        return s;
    };
    if (st.haveRequested) {
        ImGui::Text("Format: %u/%u/%u%s",
                    st.selected.sampleRate, (unsigned)st.selected.bitsPerSample,
                    (unsigned)st.selected.channels, st.selected.isFloat ? "f" : "");
    } else if (st.selected.sampleRate) {
        ImGui::Text("Format: %u/%u/%u%s (default)",
                    st.selected.sampleRate, (unsigned)st.selected.bitsPerSample,
                    (unsigned)st.selected.channels, st.selected.isFloat ? "f" : "");
    } else {
        ImGui::Text("Format: %s", wa::ui_text::kSystemDefault);
    }

    const std::string preview = (st.choiceIdx == 0) ? std::string(wa::ui_text::kSystemDefault)
                              : (st.choiceIdx >= 1) ? fmtStr(candidates[(size_t)(st.choiceIdx - 1)])
                              : fmtStr(st.selected);
    ImGui::SetNextItemWidth(-80.0f);
    if (ImGui::BeginCombo("##fmtCombo", preview.c_str())) {
        const std::string defLabel = std::string(wa::ui_text::kSystemDefault) + "##0";
        if (ImGui::Selectable(defLabel.c_str(), st.choiceIdx == 0))
            wa::create_recipe::selectDefault(st, defaultDisplay);
        for (int i = 0; i < nOk; ++i) {
            const std::string label = fmtStr(candidates[(size_t)i]) + "##" + std::to_string(i + 1);
            if (ImGui::Selectable(label.c_str(), st.choiceIdx == i + 1))
                wa::create_recipe::selectCandidate(st, candidates[(size_t)i], i + 1);
        }
        ImGui::EndCombo();
    }
    drawCaptureOptions(params);

    ImGui::SetNextItemWidth(-60.0f);
    ImGui::InputText("##fmtInput", st.custom, sizeof(st.custom));
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::kApply)) {
        std::string err;
        if (!wa::create_recipe::applyCustom(st, st.custom, &err))
            logLines_.push_back(err);
    }
    ImGui::PopID();
}

void AppUi::drawCaptureOptions(wa::StreamParams& params) {
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::kOptions))
        ImGui::OpenPopup(wa::ui_text::kCaptureOptionsPopup);
    if (!ImGui::BeginPopupModal(wa::ui_text::kCaptureOptionsPopup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextWrapped("%s", wa::ui_text::kAdvancedOptionsHelp);
    ImGui::Separator();
    ImGui::SeparatorText(wa::ui_text::kAdvancedCaptureSection);
    ImGui::PushItemWidth(190);
    drawCaptureStreamParams(params);
    ImGui::PopItemWidth();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
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
    case 0: return "Capture waveform + spectrogram";
    case 1: return "Render waveform + spectrogram (delayed)";
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
    viz.workCap.clear();
    viz.workRender.clear();
    viz.specWin.clear();
    viz.capSpecWindows.clear();
    viz.magCap.clear();
    viz.magRender.clear();
    viz.nextCapEnd = 0;
    viz.nextRenderEnd = 0;
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
        viz.xLink0 = 0.0;
        viz.xLink1 = (double)(kSpecCols * kFftHop) / (double)sr;
    }

    if (sr != viz.specSr || capChannels != viz.capSpecChannels) {
        viz.specSr = sr;
        viz.workCap.resize(kFftWin);
        if (includeRender) viz.workRender.resize(kFftWin);
        viz.specWin.resize(kFftWin);
        viz.capSpec = std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0,
                                                        (double)sr / 2.0, sr);
        if (includeRender) {
            viz.renderSpec = std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0,
                                                               (double)sr / 2.0, sr);
        }
        viz.capChannelSpecs.clear();
        viz.capChannelSpecs.reserve((size_t)capChannels);
        for (uint32_t ch = 0; ch < capChannels; ++ch) {
            viz.capChannelSpecs.push_back(std::make_unique<wa::Spectrogram>(
                kSpecRows, kSpecCols, 20.0, (double)sr / 2.0, sr));
        }
        viz.capSpecWindows.assign((size_t)capChannels, std::vector<float>(kFftWin, 0.f));
        viz.capSpecChannels = capChannels;
        viz.nextCapEnd = 0;
        if (includeRender) viz.nextRenderEnd = 0;
    } else if (includeRender && !viz.renderSpec) {
        viz.renderSpec = std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0,
                                                           (double)viz.specSr / 2.0,
                                                           viz.specSr);
    }
}

void AppUi::draw() {
    // Drain lines buffered by the logging pump thread into the panel history (bounded).
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        for (auto& l : pendingLog_) logLines_.push_back(std::move(l));
        pendingLog_.clear();
    }
    constexpr size_t kMaxLogLines = 5000;
    if (logLines_.size() > kMaxLogLines)
        logLines_.erase(logLines_.begin(),
                        logLines_.begin() + (logLines_.size() - kMaxLogLines));

    applyDumpPick();

    // Poll once; detect renderState Running->non-Running to clear stale playback chart data.
    ms_ = monitor_.poll();
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

    if (ImGui::BeginTabBar("##pages")) {
        if (ImGui::BeginTabItem("Monitor")) {
            drawMonitorPage();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Loopback")) {
            drawLoopbackPage();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(wa::ui_text::kApplicationLoopbackTab)) {
            drawApplicationLoopbackPage();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(wa::ui_text::kPipelineTab)) {
            drawPipelinePage();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void AppUi::drawMonitorPage() {
    constexpr float kLogHeight = 200.0f;
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - kLogHeight - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("monitorTop", ImVec2(0, topHeight), false);
    ImGui::BeginChild("left", ImVec2(360, 0), true);
    drawLeftPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("charts", ImVec2(0, 0), true);
    drawChartHost(&monitor_, ms_, monitorViz_, wa::chart_host::Mode::DualReorderable,
                  nullptr, &chartOrder_);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("monitorLogRegion", ImVec2(0, kLogHeight), true);
    ImGui::SeparatorText("Log");
    drawLogPanel("log", true);
    ImGui::EndChild();
}

void AppUi::drawStackedCaptureTrackHosts(wa::CaptureTrackList& list,
                                         std::vector<std::pair<wa::TrackId, VisualState>>& viz,
                                         const char* emptyHint) {
    const auto tracks = list.poll();
    for (size_t i = 0; i < viz.size();) {
        bool live = false;
        for (const auto& t : tracks) {
            if (t.id == viz[i].first) { live = true; break; }
        }
        if (!live) viz.erase(viz.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    if (tracks.empty()) {
        ImGui::TextUnformatted(emptyHint);
        return;
    }
    for (const auto& t : tracks) {
        VisualState* vs = nullptr;
        for (auto& p : viz) {
            if (p.first == t.id) { vs = &p.second; break; }
        }
        if (!vs) {
            viz.push_back({t.id, VisualState{}});
            vs = &viz.back().second;
        }
        ImGui::PushID(static_cast<int>(t.id));
        std::string cap = "Track " + std::to_string(t.id);
        if (t.source.kind == wa::CaptureSourceKind::ApplicationLoopback) {
            cap += "  pid=" + std::to_string(t.source.processId);
            cap += std::string("  ") + wa::processLoopbackModeName(t.source.processLoopbackMode);
        }
        if (t.actualFormat.sampleRate)
            cap += "  " + std::to_string(t.actualFormat.sampleRate) + " Hz / "
                + std::to_string(t.actualFormat.channels) + " ch";
        if (t.state == wa::StreamState::Error && !t.message.empty())
            cap += "  [" + t.message + "]";
        wa::TrackScopeReader reader(list, t.id);
        drawChartHost(nullptr, statusFromTrack(t), *vs, wa::chart_host::Mode::CaptureOnly,
                      cap.c_str(), nullptr, &reader);
        ImGui::PopID();
    }
}

void AppUi::drawLoopbackPage() {
    constexpr float kLogHeight = 200.0f;
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - kLogHeight - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("loopbackTop", ImVec2(0, topHeight), false);
    ImGui::BeginChild("loopbackLeft", ImVec2(320, 0), true);
    drawLoopbackLeftPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("loopbackCharts", ImVec2(0, 0), true);
    drawStackedCaptureTrackHosts(loopbackTracks_, loopbackViz_, wa::ui_text::kLoopbackEmptyHint);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("loopbackLogRegion", ImVec2(0, kLogHeight), true);
    ImGui::SeparatorText("Log");
    drawLogPanel("loopbackLog", false);
    ImGui::EndChild();
}

void AppUi::drawApplicationLoopbackPage() {
    constexpr float kLogHeight = 200.0f;
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - kLogHeight - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("appLoopbackTop", ImVec2(0, topHeight), false);
    ImGui::BeginChild("appLoopbackLeft", ImVec2(340, 0), true);
    drawApplicationLoopbackLeftPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("appLoopbackCharts", ImVec2(0, 0), true);
    drawStackedCaptureTrackHosts(appLoopbackTracks_, appLoopbackViz_,
                                 wa::ui_text::kApplicationLoopbackEmptyHint);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("appLoopbackLogRegion", ImVec2(0, kLogHeight), true);
    ImGui::SeparatorText("Log");
    drawLogPanel("appLoopbackLog", false);
    ImGui::EndChild();
}

namespace {
bool etwHintEqual(const wa::EtwInitializeHint& a, const wa::EtwInitializeHint& b) {
    return a.present == b.present && a.category == b.category && a.raw == b.raw &&
           a.matchFormat == b.matchFormat && a.exclusive == b.exclusive &&
           a.hresult == b.hresult && a.format == b.format;
}

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
    if (!pipelineEtwStarted_) {
        pipelineEtwStarted_ = true;
        wa::Result etw = pipelineEtw_.start();
        if (!etw)
            logLines_.push_back("pipeline ETW unavailable: " + etw.message);
    }
    if (!pipelineSessionsLoaded_)
        refreshPipelineSessions();
    if (pipelineSelected_ >= 0 && pipelineEtw_.status() == wa::EtwWatchStatus::Listening) {
        const wa::LiveSessionView& session = pipelineSessions_[(size_t)pipelineSelected_];
        const wa::EtwInitializeHint hint =
            wa::matchEtwInitialize(session, pipelineEtw_.snapshot(), wa::etwNowMs());
        if (!etwHintEqual(hint, pipelineEtwHint_))
            applyPipelineJoin();
    }
    if (pipelineAttach_.attached()) {
        auto more = pipelineAttach_.drain();
        if (!more.empty()) {
            pipelineCalls_.insert(pipelineCalls_.end(), more.begin(), more.end());
            applyPipelineJoin();
        }
    }

    constexpr float kLogHeight = 200.0f;
    const float availY = ImGui::GetContentRegionAvail().y;
    const float topHeight = std::max(120.0f, availY - kLogHeight - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("pipelineTop", ImVec2(0, topHeight), false);
    ImGui::BeginChild("pipelineLeft", ImVec2(420, 0), true);
    ImGui::SeparatorText(wa::ui_text::kPipelineSessions);
    if (ImGui::Button(wa::ui_text::kPipelineRefresh))
        refreshPipelineSessions();
    ImGui::SameLine();
    const bool canProbe = pipelineSelected_ >= 0 && !pipelineProbing_;
    if (!canProbe) ImGui::BeginDisabled();
    if (ImGui::Button(wa::ui_text::kPipelineProbe))
        runPipelineProbe();
    if (!canProbe) ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canAttach = pipelineSelected_ >= 0;
    if (!canAttach) ImGui::BeginDisabled();
    if (ImGui::Button(wa::ui_text::kPipelineAttach))
        runPipelineAttach();
    if (!canAttach) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox(wa::ui_text::kPipelineShowSelf, &pipelineShowSelf_))
        refreshPipelineSessions();
    const wa::EtwWatchStatus etwStatus = pipelineEtw_.status();
    if (etwStatus == wa::EtwWatchStatus::Unavailable) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.55f, 0.55f, 1.f));
        ImGui::TextUnformatted(wa::ui_text::kPipelineEtwUnavailable);
        ImGui::PopStyleColor();
    } else if (etwStatus == wa::EtwWatchStatus::Listening) {
        ImGui::TextUnformatted(wa::ui_text::kPipelineEtwListening);
    }
    if (ImGui::Checkbox(wa::ui_text::kPipelinePump, &pipelinePump_)) {
        if (pipelineAttach_.attached())
            pipelineAttach_.setPumpEnabled(pipelinePump_);
    }
    if (pipelineAttach_.attached()) {
        ImGui::TextUnformatted(wa::ui_text::kPipelineAttached);
    } else if (!pipelineAttachBanner_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.55f, 0.55f, 1.f));
        ImGui::TextWrapped("%s", pipelineAttachBanner_.c_str());
        ImGui::PopStyleColor();
    }

    if (pipelineSessions_.empty()) {
        ImGui::TextWrapped("%s", wa::ui_text::kPipelineEmpty);
    } else if (ImGui::BeginTable("pipelineSessions", 7,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Flow", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Process");
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Device");
        ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)pipelineSessions_.size(); ++i) {
            const auto& s = pipelineSessions_[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool selected = (i == pipelineSelected_);
            char label[64];
            std::snprintf(label, sizeof(label), "%s##ps%d",
                          s.flow == wa::PipelineFlow::Capture ? "capture" : "render", i);
            if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                if (pipelineSelected_ != i) {
                    pipelineProbes_.clear();
                    if (pipelineAttach_.pid() != 0 && pipelineAttach_.pid() != s.processId) {
                        pipelineAttach_.stop();
                        pipelineCalls_.clear();
                        pipelineAttachBanner_.clear();
                    }
                }
                pipelineSelected_ = i;
                rebuildPipelineGraph();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.processName.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", s.processId);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(s.deviceName.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", static_cast<double>(s.sessionVolume));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(s.sessionMute ? "yes" : "no");
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(s.state.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    const float rest = ImGui::GetContentRegionAvail().x;
    const float graphW = std::max(240.0f, rest * 0.55f);
    ImGui::BeginChild("pipelineGraph", ImVec2(graphW, 0), true);
    ImGui::SeparatorText(wa::ui_text::kPipelineGraph);
    if (pipelineSelected_ < 0) {
        ImGui::TextWrapped("%s", wa::ui_text::kPipelineSelectHint);
    } else {
        for (const auto& n : pipelineNodes_) {
            ImGui::PushStyleColor(ImGuiCol_Text, kindColor(n.kind));
            ImGui::Text("%s [%s]", n.title.c_str(), wa::observationKindName(n.kind));
            ImGui::PopStyleColor();
            for (const auto& p : n.params) {
                ImGui::PushStyleColor(ImGuiCol_Text, kindColor(p.kind));
                ImGui::BulletText("%s: %s [%s]", p.key.c_str(), p.value.c_str(),
                                  wa::observationKindName(p.kind));
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("pipelineCallLog", ImVec2(0, 0), true);
    ImGui::SeparatorText(wa::ui_text::kPipelineCallLog);
    std::vector<wa::HookedCall> callSrc = pipelineCalls_;
    if (pipelinePump_ && pipelineAttach_.attached()) {
        auto pump = pipelineAttach_.pumpRing();
        callSrc.insert(callSrc.end(), pump.begin(), pump.end());
        ImGui::Text("%s: %u", wa::ui_text::kPipelineXruns, pipelineAttach_.pumpXruns());
    }
    const auto callView = wa::shapeCallLog(callSrc, pipelinePump_);
    const auto& callLog = callView.entries;
    if (callLog.empty()) {
        ImGui::TextWrapped("%s", wa::ui_text::kPipelineCallLogEmpty);
    } else if (ImGui::BeginTable("pipelineCalls", 5,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(wa::ui_text::kPipelineCallColIface);
        ImGui::TableSetupColumn(wa::ui_text::kPipelineCallColMethod);
        ImGui::TableSetupColumn(wa::ui_text::kPipelineCallColArgs);
        ImGui::TableSetupColumn(wa::ui_text::kPipelineCallColHr, ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn(wa::ui_text::kPipelineCallColStream,
                                ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableHeadersRow();
        for (const auto& c : callLog) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(c.iface.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(c.method.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(c.args.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%ld", static_cast<long>(c.hresult));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", c.streamId);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("pipelineLogRegion", ImVec2(0, kLogHeight), true);
    ImGui::SeparatorText("Log");
    drawLogPanel("pipelineLog", false);
    ImGui::EndChild();
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
        wa::Result r = monitor_.startDumpCapture(folder);
        logStart(r, monitor_.poll().capDumpPath);
        return;
    }
    if (kind == DumpPickKind::MonitorRen) {
        wa::Result r = monitor_.startDumpRender(folder);
        logStart(r, monitor_.poll().renderDumpPath);
    }
}

void AppUi::drawDumpControls(wa::CaptureTrackList& list, const wa::CaptureTrackStatus& t,
                             DumpPickKind kind, const char* destroyedLog) {
    const bool picking = dumpPicker_.busy();
    if (t.dumping) {
        if (ImGui::Button(wa::ui_text::kDumpStop)) {
            const std::wstring path = t.dumpPath;
            wa::Result r = list.stopDump(t.id);
            logLines_.push_back(r ? ("dump stopped " + wtou(path))
                                  : ("dump stop error: " + r.message));
            if (r) wa::dump_ui::revealDumpFile(path);
        }
    } else {
        ImGui::BeginDisabled(t.state != wa::StreamState::Running || picking);
        if (ImGui::Button(wa::ui_text::kDump))
            beginDumpPick(kind, t.id);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::kLoopbackDestroy)) {
        const std::wstring dumpPath = t.dumping ? t.dumpPath : std::wstring{};
        list.destroy(t.id);
        if (!dumpPath.empty()) wa::dump_ui::revealDumpFile(dumpPath);
        logLines_.push_back(std::string(destroyedLog) + std::to_string(t.id));
    }
    if (t.dumping && !t.dumpFileName.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", wtou(t.dumpFileName).c_str());
    }
}

void AppUi::drawLoopbackLeftPanel() {
    if (!monitorDevicesLoaded_) refreshMonitorDevices();

    ImGui::SeparatorText("Source");
    if (ImGui::Button("Refresh devices")) refreshMonitorDevices();
    std::string preview = "(no render devices)";
    if (!renderDevices_.empty() && loopbackDevIdx_ >= 0 && loopbackDevIdx_ < (int)renderDevices_.size())
        preview = (renderDevices_[(size_t)loopbackDevIdx_].isDefault ? "* " : "") +
                  wtou(renderDevices_[(size_t)loopbackDevIdx_].name);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("System audio", preview.c_str())) {
        for (int i = 0; i < (int)renderDevices_.size(); ++i) {
            std::string l = (renderDevices_[(size_t)i].isDefault ? "* " : "  ") +
                            wtou(renderDevices_[(size_t)i].name);
            if (ImGui::Selectable((l + "##loopdev" + std::to_string(i)).c_str(),
                                  loopbackDevIdx_ == i))
                loopbackDevIdx_ = i;
        }
        ImGui::EndCombo();
    }

    if (loopbackRecipe_.deviceShown != loopbackDevIdx_)
        recomputeLoopbackFormat();
    drawFormatRecipe(loopbackRecipe_.format,
                     wa::create_recipe::sharedCandidates(loopbackRecipe_.caps),
                     loopbackRecipe_.caps.mixFormat, "sysLbFmt", loopbackRecipe_.params);
    ImGui::Checkbox(wa::ui_text::kLoopbackSilentRender, &loopbackSilentRender_);

    ImGui::SeparatorText("Control");
    const ImVec2 ctrlBtn(120.0f, ImGui::GetFrameHeight() * 1.3f);
    if (ImGui::Button(wa::ui_text::kLoopbackCreate, ctrlBtn)) {
        wa::DeviceId id = renderDevices_.empty() ? L"" : renderDevices_[(size_t)loopbackDevIdx_].id;
        wa::CaptureTrackCreate spec{};
        spec.kind = wa::BackendKind::WasapiShared;
        spec.source = wa::CaptureSource{wa::CaptureSourceKind::SystemLoopback, id};
        spec.loopbackOptions.silentRender = loopbackSilentRender_;
        spec.streamParams = loopbackRecipe_.params;
        spec.requested = wa::create_recipe::requestedOrNull(loopbackRecipe_.format);
        wa::TrackId tid = 0;
        wa::Result r = loopbackTracks_.create(spec, &tid);
        logLines_.push_back(r ? ("loopback track created id=" + std::to_string(tid))
                              : ("loopback error: " + r.message));
    }
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::kLoopbackDestroyAll, ctrlBtn)) {
        std::vector<std::wstring> dumpPaths;
        for (const auto& t : loopbackTracks_.poll())
            if (t.dumping) dumpPaths.push_back(t.dumpPath);
        loopbackTracks_.destroyAll();
        loopbackViz_.clear();
        for (const auto& p : dumpPaths) wa::dump_ui::revealDumpFile(p);
        logLines_.push_back("loopback destroy all");
    }

    ImGui::SeparatorText(wa::ui_text::kLoopbackTracks);
    const char* ss[] = {"Idle", "Running", "Error"};
    const auto tracks = loopbackTracks_.poll();
    for (const auto& t : tracks) {
        ImGui::PushID(static_cast<int>(t.id));
        ImGui::Text("id=%llu  %s  sr=%u  xrun=%llu",
                    (unsigned long long)t.id, ss[(int)t.state],
                    t.actualFormat.sampleRate, (unsigned long long)t.overruns);
        ImGui::ProgressBar((t.levelL > t.levelR) ? t.levelL : t.levelR, ImVec2(-1, 0), "level");
        drawDumpControls(loopbackTracks_, t, DumpPickKind::Loopback,
                         "loopback track destroyed id=");
        ImGui::PopID();
    }
}

void AppUi::drawApplicationLoopbackLeftPanel() {
    if (!appLoopbackSessionsLoaded_)
        refreshApplicationLoopbackSessions();

    ImGui::SeparatorText(wa::ui_text::kApplicationLoopbackSessions);
    if (ImGui::Button(wa::ui_text::kApplicationLoopbackRefresh))
        refreshApplicationLoopbackSessions();

    ImGui::BeginChild("appLoopbackSessionList", ImVec2(0, 170.0f), true);
    if (appLoopbackSessions_.empty()) {
        ImGui::TextUnformatted("(none)");
    } else {
        for (int i = 0; i < (int)appLoopbackSessions_.size(); ++i) {
            const auto& row = appLoopbackSessions_[(size_t)i];
            std::string label = wtou(row.processName) + "  " + std::to_string(row.processId) +
                                "##appLoopbackSession" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), appLoopbackSessionIdx_ == i)) {
                appLoopbackSessionIdx_ = i;
                wa::app_loopback_ui::copySessionPidToBuffer(appLoopbackSessions_, i,
                                                            appLoopbackPid_,
                                                            sizeof(appLoopbackPid_));
            }
        }
    }
    ImGui::EndChild();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(wa::ui_text::kApplicationLoopbackPidLabel);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("##appLoopbackPid", appLoopbackPid_, sizeof(appLoopbackPid_));
    ImGui::SameLine();
    ImGui::Checkbox(wa::ui_text::kApplicationLoopbackExclude, &appLoopbackExclude_);
    drawFormatRecipe(appLoopbackRecipe_.format, {}, wa::AudioFormat{}, "appLbFmt",
                     appLoopbackRecipe_.params);

    ImGui::SeparatorText("Control");
    const ImVec2 ctrlBtn(120.0f, ImGui::GetFrameHeight() * 1.3f);
    if (ImGui::Button(wa::ui_text::kLoopbackCreate, ctrlBtn)) {
        uint32_t pid = 0;
        if (!wa::parseApplicationLoopbackPid(appLoopbackPid_, pid)) {
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
            logLines_.push_back(r
                ? ("application loopback track created id=" + std::to_string(tid)
                   + " pid=" + std::to_string(pid)
                   + " mode=" + wa::processLoopbackModeName(mode))
                : ("application loopback error: " + r.message));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::kLoopbackDestroyAll, ctrlBtn)) {
        std::vector<std::wstring> dumpPaths;
        for (const auto& t : appLoopbackTracks_.poll())
            if (t.dumping) dumpPaths.push_back(t.dumpPath);
        appLoopbackTracks_.destroyAll();
        appLoopbackViz_.clear();
        for (const auto& p : dumpPaths) wa::dump_ui::revealDumpFile(p);
        logLines_.push_back("application loopback destroy all");
    }

    ImGui::SeparatorText(wa::ui_text::kLoopbackTracks);
    const char* ss[] = {"Idle", "Running", "Error"};
    const auto tracks = appLoopbackTracks_.poll();
    for (const auto& t : tracks) {
        ImGui::PushID(static_cast<int>(t.id));
        ImGui::Text("id=%llu  %s  pid=%u  %s  sr=%u  xrun=%llu",
                    (unsigned long long)t.id, ss[(int)t.state],
                    t.source.processId,
                    wa::processLoopbackModeName(t.source.processLoopbackMode),
                    t.actualFormat.sampleRate, (unsigned long long)t.overruns);
        ImGui::ProgressBar((t.levelL > t.levelR) ? t.levelL : t.levelR, ImVec2(-1, 0), "level");
        drawDumpControls(appLoopbackTracks_, t, DumpPickKind::AppLoopback,
                         "application loopback track destroyed id=");
        ImGui::PopID();
    }
}

void AppUi::drawLeftPanel() {
    if (!monitorDevicesLoaded_) refreshMonitorDevices();

    // --- Devices ---
    ImGui::SeparatorText("Devices");
    if (ImGui::Button("Refresh devices")) refreshMonitorDevices();
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::kOptions)) ImGui::OpenPopup("Audio parameters (advanced)");
    if (ImGui::Button("Capture caps\xe2\x80\xa6")) {   // U+2026 HORIZONTAL ELLIPSIS
        wa::ComInitGuard com;
        wa::DeviceId capId = (capDevIdx_ >= 0 && capDevIdx_ < (int)capDevices_.size())
                             ? capDevices_[(size_t)capDevIdx_].id : L"";
        enumerator_.queryCapabilities(wa::DataFlow::Capture, capId, capsCache_);
        ImGui::OpenPopup("Capture capabilities");
    }

    auto deviceCombo = [&](const char* caption, const char* comboId,
                           const std::vector<wa::DeviceInfo>& devs, int& idx) {
        ImGui::TextUnformatted(caption);
        std::string preview = "(no devices)";
        if (!devs.empty() && idx >= 0 && idx < (int)devs.size())
            preview = (devs[(size_t)idx].isDefault ? "* " : "") + wtou(devs[(size_t)idx].name);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(comboId, preview.c_str())) {
            for (int i = 0; i < (int)devs.size(); ++i) {
                std::string l = (devs[(size_t)i].isDefault ? "* " : "  ") + wtou(devs[(size_t)i].name);
                if (ImGui::Selectable((l + "##" + std::to_string(i)).c_str(), idx == i))
                    idx = i;
            }
            ImGui::EndCombo();
        }
    };
    deviceCombo("Capture device", "##capdev", capDevices_, capDevIdx_);
    deviceCombo("Render device",  "##rendev", renderDevices_, renderDevIdx_);
    drawAdvancedModal();
    drawCapsModal();

    // --- Control ---
    ImGui::SeparatorText("Control");
    const char* backends[] = {"WASAPI-Shared", "WASAPI-Exclusive"};
    ImGui::Combo("Backend", &backendIdx_, backends, 2);
    ImGui::SliderInt("Delay (ms)", &delayMs_, 0, 500);
    drawFormatRegion();

    const ImVec2 ctrlBtn(120.0f, ImGui::GetFrameHeight() * 1.3f);   // enlarged Start/Stop button
    if (!monitorStarted_) {
        if (ImGui::Button("Start", ctrlBtn)) {
            wa::DeviceId capId = capDevices_.empty()    ? L"" : capDevices_[capDevIdx_].id;
            wa::DeviceId renId = renderDevices_.empty() ? L"" : renderDevices_[renderDevIdx_].id;
            wa::BackendKind kind = (backendIdx_ == 1) ? wa::BackendKind::WasapiExclusive
                                                      : wa::BackendKind::WasapiShared;
            wa::Result r = monitor_.start(kind, capId, renId, (uint32_t)delayMs_,
                                          playbackEnabled_, capParams_, renParams_,
                                          haveFmt_ ? &selectedFmt_ : nullptr);
            logLines_.push_back(r ? "monitor started" : ("monitor error: " + r.message));
            if (r) {
                monitorStarted_ = true;
                resetVisuals(monitorViz_);
            }
        }
    } else {
        if (ImGui::Button("Stop", ctrlBtn)) {
            const std::wstring capDump = ms_.capDumping ? ms_.capDumpPath : std::wstring{};
            const std::wstring renDump = ms_.renderDumping ? ms_.renderDumpPath : std::wstring{};
            monitor_.stop();
            monitorStarted_ = false;
            resetVisuals(monitorViz_);
            if (!capDump.empty()) wa::dump_ui::revealDumpFile(capDump);
            if (!renDump.empty()) wa::dump_ui::revealDumpFile(renDump);
            logLines_.push_back("monitor stopped");
        }
    }
    ImGui::SameLine();
    // vertically center the playback checkbox against the taller Start/Stop button
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ctrlBtn.y - ImGui::GetFrameHeight()) * 0.5f);

    // Playback checkbox — always toggleable; applies live while running, else takes effect on next Start.
    if (ImGui::Checkbox("同步播放 (playback)", &playbackEnabled_)) {
        if (monitorStarted_ && !playbackEnabled_ && ms_.renderDumping) {
            const std::wstring path = ms_.renderDumpPath;
            wa::Result r = monitor_.stopDumpRender();
            logLines_.push_back(r ? ("dump stopped " + wtou(path))
                                  : ("dump stop error: " + r.message));
            if (r) wa::dump_ui::revealDumpFile(path);
        }
        if (monitorStarted_) monitor_.setPlaybackEnabled(playbackEnabled_);
    }

    const bool dumpPicking = dumpPicker_.busy();
    ImGui::BeginDisabled(!monitorStarted_ || ms_.capState != wa::StreamState::Running);
    ImGui::PushID("capDump");
    if (ms_.capDumping) {
        if (ImGui::Button(wa::ui_text::kDumpStop)) {
            const std::wstring path = ms_.capDumpPath;
            wa::Result r = monitor_.stopDumpCapture();
            logLines_.push_back(r ? ("dump stopped " + wtou(path))
                                  : ("dump stop error: " + r.message));
            if (r) wa::dump_ui::revealDumpFile(path);
        }
        if (!ms_.capDumpFileName.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", wtou(ms_.capDumpFileName).c_str());
        }
    } else {
        ImGui::BeginDisabled(dumpPicking);
        if (ImGui::Button(wa::ui_text::kDumpCapture))
            beginDumpPick(DumpPickKind::MonitorCap);
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!monitorStarted_ || !playbackEnabled_
                         || ms_.renderState != wa::StreamState::Running);
    ImGui::PushID("renDump");
    if (ms_.renderDumping) {
        if (ImGui::Button(wa::ui_text::kDumpStop)) {
            const std::wstring path = ms_.renderDumpPath;
            wa::Result r = monitor_.stopDumpRender();
            logLines_.push_back(r ? ("dump stopped " + wtou(path))
                                  : ("dump stop error: " + r.message));
            if (r) wa::dump_ui::revealDumpFile(path);
        }
        if (!ms_.renderDumpFileName.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", wtou(ms_.renderDumpFileName).c_str());
        }
    } else {
        ImGui::BeginDisabled(dumpPicking);
        if (ImGui::Button(wa::ui_text::kDumpRender))
            beginDumpPick(DumpPickKind::MonitorRen);
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    ImGui::EndDisabled();

    // --- Status ---
    ImGui::SeparatorText("Status");
    const char* ss[] = {"Idle", "Running", "Error"};
    ImGui::Text("overall=%s  cap=%s  ren=%s  sr=%u  delay=%ums",
        ss[(int)ms_.overall], ss[(int)ms_.capState], ss[(int)ms_.renderState],
        ms_.sampleRate, ms_.delayMs);
    ImGui::Text("fifo=%.0fms  drift=%llu  xrun c/r=%llu/%llu",
        ms_.fifoFillMs,
        (unsigned long long)ms_.driftFixes,
        (unsigned long long)ms_.capXruns,
        (unsigned long long)ms_.renderXruns);
    ImGui::Text("frames c/r=%llu/%llu",
        (unsigned long long)ms_.capWrittenFrames,
        (unsigned long long)ms_.renderWrittenFrames);
    ImGui::ProgressBar(ms_.capLevel,    ImVec2(-1, 0), "cap");
    ImGui::ProgressBar(ms_.renderLevel, ImVec2(-1, 0), "ren");

}

void AppUi::drawLogPanel(const char* childId, bool showLevelFilter) {
    if (showLevelFilter) {
        static const char* kLevels[] = {"Trace", "Debug", "Info", "Warn", "Err"};
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::Combo("##loglevel", &logLevelIdx_, kLevels, IM_ARRAYSIZE(kLevels)))
            wa::log::setLevel(static_cast<wa::log::Level>(logLevelIdx_));
        ImGui::SameLine();
        if (ImGui::Button("Clear##mainLog")) logLines_.clear();
    } else {
        if (ImGui::Button("Clear##loopbackLog")) logLines_.clear();
    }

    ImGui::BeginChild(childId, ImVec2(0, 0), true);
    const bool wasPinned = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    ImGui::PushTextWrapPos(0.0f);
    for (const auto& l : logLines_) ImGui::TextUnformatted(l.c_str());
    ImGui::PopTextWrapPos();
    if (wasPinned)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void AppUi::drawAdvancedModal() {
    if (!ImGui::BeginPopupModal("Audio parameters (advanced)", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("%s", wa::ui_text::kAdvancedOptionsHelp);
    if (monitorStarted_)
        ImGui::TextColored(ImVec4(1.00f, 0.80f, 0.30f, 1.00f),
                           "Running: parameters are read-only. Stop the monitor to change them.");
    ImGui::Separator();

    ImGui::BeginDisabled(monitorStarted_);              // view-only while running

    ImGui::BeginGroup();                              // ---- Capture column
    ImGui::SeparatorText(wa::ui_text::kAdvancedCaptureSection);
    ImGui::PushID("capP");
    ImGui::PushItemWidth(190);
    drawCaptureStreamParams(capParams_);
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::EndGroup();

    ImGui::SameLine(0, 24);

    ImGui::BeginGroup();                              // ---- Render column
    ImGui::SeparatorText(wa::ui_text::kAdvancedRenderSection);
    ImGui::PushID("renP");
    ImGui::PushItemWidth(190);
    bool renProps = renParams_.clientProperties.enabled;
    if (ImGui::Checkbox(wa::ui_text::kAdvancedSetClientProperties, &renProps))
        renParams_.clientProperties.enabled = renProps;
    ImGui::BeginDisabled(!renParams_.clientProperties.enabled);
    int v = (int)renParams_.clientProperties.category;
    if (ImGui::Combo("Category", &v, kAudioCategories, IM_ARRAYSIZE(kAudioCategories)))
        renParams_.clientProperties.category = (wa::AudioCategory)v;
    bool off = renParams_.clientProperties.offload;
    if (ImGui::Checkbox(wa::ui_text::kAdvancedHardwareOffload, &off))
        renParams_.clientProperties.offload = off;
    v = (int)renParams_.clientProperties.option;
    if (ImGui::Combo("Stream option", &v, wa::ui_text::kAdvancedStreamOptions,
                     wa::ui_text::kAdvancedStreamOptionCount))
        renParams_.clientProperties.option = (wa::StreamOption)v;
    ImGui::EndDisabled();
    bool duck = renParams_.ducking == wa::DuckingMode::OptOut;
    if (ImGui::Checkbox(wa::ui_text::kAdvancedDuckingOptOut, &duck)) renParams_.ducking = duck ? wa::DuckingMode::OptOut : wa::DuckingMode::Default;
    v = (int)renParams_.bufferMs;
    if (ImGui::InputInt("Buffer (ms)", &v)) renParams_.bufferMs = (uint32_t)std::clamp(v, 0, 2000);
    if (ImGui::Button("Reset to system defaults")) renParams_ = wa::StreamParams{};
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::EndGroup();

    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void AppUi::drawChartsFreezeToolbar(VisualState& viz, const wa::MonitorStatus& status) {
    // Shared by Monitor / Loopback / Application Loopback: freeze + time zoom / reset.
    const bool overallRunning =
        (status.overall == wa::StreamState::Running && status.sampleRate > 0);
    viz.chartsFrozen = wa::charts_freeze::applyLifecycle(overallRunning, viz.chartsFrozen);

    ImGui::BeginDisabled(!wa::charts_freeze::isControlEnabled(overallRunning));
    if (ImGui::Button(viz.chartsFrozen ? wa::ui_text::kChartsResume : wa::ui_text::kChartsPause))
        viz.chartsFrozen = !viz.chartsFrozen;
    ImGui::EndDisabled();
    if (viz.chartsFrozen) {
        ImGui::SameLine();
        ImGui::TextUnformatted(wa::ui_text::kChartsPaused);
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
    if (ImGui::Button(wa::ui_text::kChartsZoomOut)) {
        const auto z = wa::charts_time_zoom::zoomCentered(viz.xLink0, viz.xLink1, historyH, hopSec, 2.0);
        viz.xLink0 = z.x0;
        viz.xLink1 = z.x1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(
        !wa::charts_time_zoom::canZoomIn(viz.xLink0, viz.xLink1, historyH, hopSec));
    if (ImGui::Button(wa::ui_text::kChartsZoomIn)) {
        const auto z = wa::charts_time_zoom::zoomCentered(viz.xLink0, viz.xLink1, historyH, hopSec, 0.5);
        viz.xLink0 = z.x0;
        viz.xLink1 = z.x1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(wa::ui_text::kChartsResetView)) {
        const auto full = wa::charts_time_zoom::fullHistory(historyH);
        viz.xLink0 = full.x0;
        viz.xLink1 = full.x1;
        viz.resetYAxes = true; // applied once by each plot this frame; cleared after charts draw
    }
}

void AppUi::drawChartHost(wa::MonitorEngine* engine, const wa::MonitorStatus& status,
                          VisualState& viz, wa::chart_host::Mode mode, const char* caption,
                          std::vector<int>* order, wa::ScopeReader* captureReader) {
    // Chart Host: ensure → freeze/zoom toolbar → panels → clear one-shot Y reset.
    const bool includeRender = (mode == wa::chart_host::Mode::DualReorderable);
    ensureRunningVisuals(status, viz, includeRender);
    drawChartsFreezeToolbar(viz, status);

    if (caption && caption[0] != '\0')
        ImGui::TextUnformatted(caption);

    if (mode == wa::chart_host::Mode::DualReorderable) {
        // Sanitize / complete order each frame; reorder UI mutates and writes back if order != null.
        std::vector<int> panelOrder =
            wa::chart_host::resolvePanelIds(mode, order ? *order : std::vector<int>{});
        if (order) *order = panelOrder;

        for (int pos = 0; pos < (int)panelOrder.size(); ++pos) {
            const int id = panelOrder[(size_t)pos];
            if (!engine) continue;
            ImGui::PushID(pos);
            const float bh = ImGui::GetFrameHeight();
            ImGui::Button("##drag", ImVec2(bh, bh));
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            const ImVec2 hmn = ImGui::GetItemRectMin(), hmx = ImGui::GetItemRectMax();
            drawMoveIcon(ImGui::GetWindowDrawList(),
                         ImVec2((hmn.x + hmx.x) * 0.5f, (hmn.y + hmx.y) * 0.5f),
                         ImGui::GetFontSize(), ImGui::GetColorU32(ImGuiCol_Text));
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("CHART_POS", &pos, sizeof(int));
                ImGui::TextUnformatted(chartTitle(id));
                ImGui::EndDragDropSource();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(chartTitle(id));
            wa::MonitorScopeReader cap(*engine, wa::MonitorScopeReader::Side::Capture);
            wa::MonitorScopeReader ren(*engine, wa::MonitorScopeReader::Side::Render);
            drawChartPanel(id, cap, &ren, status, viz);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("CHART_POS")) {
                    const int from = *(const int*)pl->Data;
                    if (from != pos && from >= 0 && from < (int)panelOrder.size()) {
                        const int moved = panelOrder[(size_t)from];
                        panelOrder.erase(panelOrder.begin() + from);
                        panelOrder.insert(panelOrder.begin() + pos, moved);
                        if (order) *order = panelOrder;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        }
    } else {
        const auto ids = wa::chart_host::resolvePanelIds(mode, {});
        if (captureReader) {
            for (int id : ids)
                drawChartPanel(id, *captureReader, nullptr, status, viz);
        } else if (engine) {
            wa::MonitorScopeReader ownedCap(*engine, wa::MonitorScopeReader::Side::Capture);
            for (int id : ids)
                drawChartPanel(id, ownedCap, nullptr, status, viz);
        }
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
void AppUi::drawSpectrogramPanel(VisualState& viz, const char* plotId, wa::Spectrogram* spec,
                                 double histSec, float height, int slot) {
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
            tickV[nTick] = std::log10(kFreq[i]); tickL[nTick] = kFreqL[i]; ++nTick;
        }
    ImPlot::PushColormap(waSpectroColormap());   // Plasma-like, transparent at silence (grid shows)
    if (ImPlot::BeginPlot(plotId, ImVec2(-1, height))) {
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
        if (spec)
            ImPlot::PlotHeatmap("##hm", spec->data(), spec->rows(), spec->cols(), -96.0, 0.0, nullptr,
                ImPlotPoint(0, loL), ImPlotPoint(histSec, hiL));
        drawYUnitLabel("Hz", true);
        viz.plotHovPrev[slot] = ImPlot::IsPlotHovered();
        ImPlot::EndPlot();
    }
    ImPlot::PopColormap();
}

// Draw one waveform over the shared time axis (xLink0_..xLink1_ seconds). wave[i] is at t = i/sr,
// so the buffer's tail holds the newest samples (right edge). Level-of-detail: zoomed in -> raw
// line; zoomed out -> per-pixel min/max envelope with translucent fill + opaque outline so far
// view stays readable while remaining column-aligned with the spectrogram.
void AppUi::drawWaveformPanel(VisualState& viz, const char* plotId, const float* wave, int n,
                              uint32_t sr, bool haveData, float height, int slot) {
    if (!ImPlot::BeginPlot(plotId, ImVec2(-1, height))) return;
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
    ImPlot::SetupAxisTicks(ImAxis_Y1, kAmpV, 11, kAmpL);
    if (haveData && n > 0 && sr > 0) {
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
                viz.envMax.resize((size_t)visN);                     // scratch: warped raw samples
                for (int i = 0; i < visN; ++i) viz.envMax[(size_t)i] = dbWarp(wave[iLo + i]);
                ImPlot::SetNextLineStyle(kWaveColor);
                ImPlot::PlotLine("##wave", viz.envMax.data(), visN, invSr, (double)iLo * invSr);
            } else {
                const int cols = (int)pw;
                viz.envX.resize((size_t)cols); viz.envMin.resize((size_t)cols); viz.envMax.resize((size_t)cols);
                for (int c = 0; c < cols; ++c) {
                    int s0 = iLo + (int)((int64_t)visN * c / cols);
                    int s1 = iLo + (int)((int64_t)visN * (c + 1) / cols);
                    if (s1 <= s0) s1 = s0 + 1;
                    if (s1 > n)   s1 = n;
                    float mn = wave[s0], mx = wave[s0];
                    for (int s = s0 + 1; s < s1; ++s) { mn = std::min(mn, wave[s]); mx = std::max(mx, wave[s]); }
                    viz.envX[(size_t)c]   = (float)(0.5 * (double)(s0 + s1) * invSr);
                    viz.envMin[(size_t)c] = dbWarp(mn);              // warp commutes with min/max
                    viz.envMax[(size_t)c] = dbWarp(mx);
                }
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
    const uint32_t sr         = status.sampleRate;
    const bool overallRunning = (status.overall     == wa::StreamState::Running && sr > 0);
    const bool renderRunning  = (status.renderState == wa::StreamState::Running);
    const auto capturePlan = wa::capture_channel_view::makePlan(status.captureChannels);
    // Split layout follows the capture plan whenever the page is running (including frozen),
    // so multi-channel panels stay mounted while chart data is held.
    const bool splitCapture = !renderSide && overallRunning && capturePlan.split;

    // Chart Data Pipeline: hop-aligned spectrum + latest wave history; freeze gated inside.
    wa::ChartRefreshParams refreshParams;
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
            ImGui::Text("Capture channels: %u / %u channels shown", shownChannels, actualChannels);

        wa::ChartBuffers buffers;
        buffers.channelWaves = &viz.capChannelWaves;
        buffers.channelCount = shownChannels;
        buffers.waveN = viz.waveN;
        buffers.waveSr = viz.waveSr;
        buffers.channelSpecWindows = &viz.capSpecWindows;
        buffers.channelSpecs = &viz.capChannelSpecs;
        buffers.work = &viz.workCap;
        buffers.mag = &viz.magCap;
        buffers.nextEnd = &viz.nextCapEnd;
        buffers.specSr = viz.specSr;
        const wa::ChartRefreshResult refreshed = wa::refreshCharts(refreshParams, buffers);

        for (uint32_t ch = 0; ch < shownChannels; ++ch) {
            std::vector<float>& wave = viz.capChannelWaves[(size_t)ch];
            const std::string title = "Capture Ch " + std::to_string(ch + 1u) + " waveform";
            ImGui::TextUnformatted(title.c_str());
            const std::string plotId = "##capWaveCh" + std::to_string(ch);
            drawWaveformPanel(viz, plotId.c_str(), wave.data(), viz.waveN, viz.waveSr,
                              refreshed.haveWave, kMultiWaveH, kSlotCapWaveBase + (int)ch);
        }

        const uint32_t hz = (sr > 0) ? sr : 48000u;
        const double histSec = (double)(kSpecCols * kFftHop) / (double)hz;
        for (uint32_t ch = 0; ch < shownChannels; ++ch) {
            const std::string title = "Capture Ch " + std::to_string(ch + 1u) + " spectrogram";
            ImGui::TextUnformatted(title.c_str());
            const std::string plotId = "##capSpecCh" + std::to_string(ch);
            wa::Spectrogram* spec = (ch < viz.capChannelSpecs.size())
                                        ? viz.capChannelSpecs[(size_t)ch].get()
                                        : nullptr;
            drawSpectrogramPanel(viz, plotId.c_str(), spec, histSec, kMultiSpecH,
                                 kSlotCapSpectroBase + (int)ch);
        }
        return;
    }

    std::vector<float>& wave = renderSide ? viz.renderWave : viz.capWave;
    wa::ChartBuffers buffers;
    buffers.wave = wave.empty() ? nullptr : wave.data();
    buffers.waveN = viz.waveN;
    buffers.waveSr = viz.waveSr;
    buffers.specWin = &viz.specWin;
    buffers.work = renderSide ? &viz.workRender : &viz.workCap;
    buffers.mag = renderSide ? &viz.magRender : &viz.magCap;
    buffers.spectrogram = renderSide ? viz.renderSpec.get() : viz.capSpec.get();
    buffers.nextEnd = renderSide ? &viz.nextRenderEnd : &viz.nextCapEnd;
    buffers.specSr = viz.specSr;
    const wa::ChartRefreshResult refreshed = wa::refreshCharts(refreshParams, buffers);
    bool ok = refreshed.haveWave;
    if (!ok && viz.chartsFrozen && viz.waveSr > 0 && viz.waveN > 0 &&
        (!renderSide || !viz.renderWave.empty()))
        ok = true;

    const float waveH = kComboH * viz.comboRatio;
    drawWaveformPanel(viz, renderSide ? "##renWave" : "##capWave", wave.data(), viz.waveN, viz.waveSr, ok,
                      waveH, renderSide ? kSlotRenWave : kSlotCapWaveBase);

    // Splitter: drag to rebalance waveform vs spectrogram height (the axes re-lay out to fit).
    ImGui::InvisibleButton(renderSide ? "##renSplit" : "##capSplit",
                           ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), kSplitH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive())
        viz.comboRatio = std::clamp(viz.comboRatio + ImGui::GetIO().MouseDelta.y / kComboH, 0.15f, 0.85f);
    const ImVec2 smn = ImGui::GetItemRectMin(), smx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(smn.x, (smn.y + smx.y) * 0.5f),
                                        ImVec2(smx.x, (smn.y + smx.y) * 0.5f),
                                        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);

    wa::Spectrogram* spec = nullptr;
    if (overallRunning) {
        // Live: render spectrogram only while playback runs. Frozen: keep last renderSpec even
        // if playback was turned off mid-freeze (resetRenderVisuals is suppressed).
        if (renderSide)
            spec = (renderRunning || viz.chartsFrozen) ? viz.renderSpec.get() : nullptr;
        else
            spec = viz.capSpec.get();
    }
    const uint32_t hz = (sr > 0) ? sr : 48000u;   // idle: assume 48 kHz for the axis ranges
    drawSpectrogramPanel(viz, renderSide ? "##renSpec" : "##capSpec", spec,
                         (double)(kSpecCols * kFftHop) / (double)hz, kComboH - waveH,
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
    if (!ImGui::BeginPopupModal("Capture capabilities", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    // Helper: format an AudioFormat as "sr/bits/ch[f]" or em-dash if not present.
    // U+2014 EM DASH is in General Punctuation (0x2000-0x206F) included by
    // GetGlyphRangesChineseSimplifiedCommon(), so it renders with the loaded font.
    auto fmtStr = [](const wa::AudioFormat& f, bool has) -> std::string {
        if (!has) return "\xe2\x80\x94";   // U+2014 EM DASH
        std::string s = std::to_string(f.sampleRate) + "/" +
                        std::to_string(f.bitsPerSample) + "/" +
                        std::to_string(f.channels);
        if (f.isFloat) s += "f";
        return s;
    };

    // --- Top: three format sources side by side (Mix / Device / OEM) ---
    if (ImGui::BeginTable("sources", 3, ImGuiTableFlags_BordersOuter)) {
        ImGui::TableSetupColumn("Mix");
        ImGui::TableSetupColumn("Device");
        ImGui::TableSetupColumn("OEM");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(fmtStr(capsCache_.mixFormat,    capsCache_.hasMix).c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(fmtStr(capsCache_.deviceFormat, capsCache_.hasDevice).c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(fmtStr(capsCache_.oemFormat,    capsCache_.hasOem).c_str());
        ImGui::EndTable();
    }
    ImGui::Spacing();

    // --- One-dimensional matrix: one row per format, fixed-height scrollable child ---
    // Note: U+2713 CHECK MARK / U+2717 BALLOT X are in Dingbats (0x2700-0x27BF), outside the
    // default Chinese glyph range; they render as replacement boxes if the glyph atlas does not
    // include that block.  The table structure remains clear regardless.
    ImGui::BeginChild("##capsScroll", ImVec2(400.0f, 300.0f), false);
    if (ImGui::BeginTable("caps", 3,
                          ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Format");
        ImGui::TableSetupColumn("Shared");
        ImGui::TableSetupColumn("Exclusive");
        ImGui::TableHeadersRow();
        for (const auto& fs : capsCache_.matrix) {
            ImGui::TableNextRow();
            std::string fmt = std::to_string(fs.fmt.sampleRate) + "/" +
                              std::to_string(fs.fmt.bitsPerSample) + "/" +
                              std::to_string(fs.fmt.channels);
            if (fs.fmt.isFloat) fmt += "f";
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(fmt.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(fs.sharedOk    ? "yes" : "-");
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(fs.exclusiveOk ? "yes" : "-");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
