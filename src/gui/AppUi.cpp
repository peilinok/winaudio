#include "AppUi.h"
#include "ApplicationLoopbackUiModel.h"
#include "AppUiText.h"
#include "CaptureChannelView.h"
#include "ChartsFreezePolicy.h"
#include "ChartsTimeZoomPolicy.h"
#include "ComUtil.h"
#include "imgui.h"
#include "implot.h"
#include "Fft.h"
#include "Analysis.h"
#include "FormatSpec.h"
#include "Log.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>

static std::string wtou(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
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
    monitorDevicesLoaded_ = true;
}

void AppUi::stopAll() {
    if (appLoopbackStartPending_) {
        if (appLoopbackStartThread_.joinable())
            appLoopbackStartThread_.detach();
        appLoopbackStartPending_ = false;
        appLoopbackStartJob_.reset();
    } else if (appLoopbackStartThread_.joinable()) {
        appLoopbackStartThread_.join();
    }
    monitor_.stop();
    loopback_.stop();
    if (appLoopback_) appLoopback_->stop();
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

void AppUi::beginApplicationLoopbackStart(uint32_t pid, wa::ProcessLoopbackMode mode) {
    if (appLoopbackStartThread_.joinable())
        appLoopbackStartThread_.join();

    auto job = std::make_shared<AppLoopbackStartJob>();
    appLoopbackStartJob_ = job;
    appLoopbackStartPending_ = true;
    appLoopbackMode_ = mode;

    appLoopbackStartThread_ = std::thread([job, pid, mode] {
        auto engine = std::make_unique<wa::MonitorEngine>();
        wa::CaptureSource source{wa::CaptureSourceKind::ApplicationLoopback, L"", pid, mode};
        wa::Result r = engine->start(wa::BackendKind::WasapiShared, source, L"", 0,
                                     false, {}, {}, nullptr, {});
        std::lock_guard<std::mutex> lk(job->mtx);
        job->result = std::move(r);
        if (job->result)
            job->engine = std::move(engine);
        job->done = true;
    });
}

void AppUi::drainApplicationLoopbackStart() {
    if (!appLoopbackStartPending_ || !appLoopbackStartJob_)
        return;

    auto job = appLoopbackStartJob_;
    wa::Result result = wa::Result::Ok();
    std::unique_ptr<wa::MonitorEngine> startedEngine;
    {
        std::lock_guard<std::mutex> lk(job->mtx);
        if (!job->done)
            return;
        result = job->result;
        if (result)
            startedEngine = std::move(job->engine);
    }

    if (appLoopbackStartThread_.joinable())
        appLoopbackStartThread_.join();
    appLoopbackStartPending_ = false;
    appLoopbackStartJob_.reset();

    if (result && startedEngine) {
        if (appLoopback_)
            appLoopback_->stop();
        appLoopback_ = std::move(startedEngine);
        appLoopbackStarted_ = true;
        resetVisuals(appLoopbackViz_);
        logLines_.push_back("application loopback started");
    } else {
        appLoopbackStarted_ = false;
        logLines_.push_back("application loopback error: " +
                            (result ? std::string("start completed without engine")
                                    : result.message));
    }
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
    if (ImGui::Button("Apply")) {
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

    // Poll once; detect renderState Running->non-Running to clear stale playback chart data.
    drainApplicationLoopbackStart();
    ms_ = monitor_.poll();
    loopbackMs_ = loopback_.poll();
    if (!appLoopbackStartPending_ && appLoopback_)
        appLoopbackMs_ = appLoopback_->poll();
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
    drawChartsColumn(monitor_, ms_, monitorViz_);
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("monitorLogRegion", ImVec2(0, kLogHeight), true);
    ImGui::SeparatorText("Log");
    drawLogPanel("log", true);
    ImGui::EndChild();
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
    ensureRunningVisuals(loopbackMs_, loopbackViz_, false);
    drawChartsFreezeToolbar(loopbackViz_, loopbackMs_);
    ImGui::TextUnformatted("System audio waveform + spectrogram");
    drawChartPanel(0, loopback_, loopbackMs_, loopbackViz_);
    loopbackViz_.resetYAxes = false;
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
    ensureRunningVisuals(appLoopbackMs_, appLoopbackViz_, false);
    drawChartsFreezeToolbar(appLoopbackViz_, appLoopbackMs_);
    ImGui::TextUnformatted("Application audio waveform + spectrogram");
    if (appLoopback_)
        drawChartPanel(0, *appLoopback_, appLoopbackMs_, appLoopbackViz_);
    appLoopbackViz_.resetYAxes = false;
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::BeginChild("appLoopbackLogRegion", ImVec2(0, kLogHeight), true);
    ImGui::SeparatorText("Log");
    drawLogPanel("appLoopbackLog", false);
    ImGui::EndChild();
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

    ImGui::SeparatorText("Control");
    const ImVec2 ctrlBtn(120.0f, ImGui::GetFrameHeight() * 1.3f);
    if (!loopbackStarted_) {
        if (ImGui::Button("Start", ctrlBtn)) {
            wa::DeviceId id = renderDevices_.empty() ? L"" : renderDevices_[(size_t)loopbackDevIdx_].id;
            wa::CaptureSource source{wa::CaptureSourceKind::SystemLoopback, id};
            wa::LoopbackOptions loopbackOptions{};
            loopbackOptions.silentRender = loopbackSilentRender_;
            wa::Result r = loopback_.start(wa::BackendKind::WasapiShared, source, L"", 0,
                                           false, {}, {}, nullptr, loopbackOptions);
            logLines_.push_back(r ? "loopback started" : ("loopback error: " + r.message));
            if (r) {
                loopbackStarted_ = true;
                resetVisuals(loopbackViz_);
            }
        }
    } else {
        if (ImGui::Button("Stop", ctrlBtn)) {
            loopback_.stop();
            loopbackStarted_ = false;
            resetVisuals(loopbackViz_);
            logLines_.push_back("loopback stopped");
        }
    }
    if (!loopbackStarted_) {
        ImGui::Checkbox("Silent render keepalive", &loopbackSilentRender_);
    } else {
        ImGui::BeginDisabled();
        ImGui::Checkbox("Silent render keepalive", &loopbackSilentRender_);
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Status");
    const char* ss[] = {"Idle", "Running", "Error"};
    ImGui::Text("overall=%s  cap=%s  sr=%u",
        ss[(int)loopbackMs_.overall], ss[(int)loopbackMs_.capState], loopbackMs_.sampleRate);
    ImGui::Text("xrun=%llu", (unsigned long long)loopbackMs_.capXruns);
    ImGui::Text("frames=%llu", (unsigned long long)loopbackMs_.capWrittenFrames);
    ImGui::Text("silent-packet=%llu  idle-fill=%llu",
        (unsigned long long)loopbackMs_.loopbackSilentPacketFrames,
        (unsigned long long)loopbackMs_.loopbackIdleSilenceFrames);
    ImGui::Text("silent render=%s",
        ss[(int)loopbackMs_.silentRenderState]);
    ImGui::ProgressBar(loopbackMs_.capLevel, ImVec2(-1, 0), "level");

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

    ImGui::SeparatorText("Control");
    const bool freezeParams = appLoopbackStartPending_ || appLoopbackStarted_;
    if (freezeParams) ImGui::BeginDisabled();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(wa::ui_text::kApplicationLoopbackPidLabel);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("##appLoopbackPid", appLoopbackPid_, sizeof(appLoopbackPid_));
    ImGui::SameLine();
    ImGui::Checkbox(wa::ui_text::kApplicationLoopbackExclude, &appLoopbackExclude_);
    if (freezeParams) ImGui::EndDisabled();

    const ImVec2 ctrlBtn(120.0f, ImGui::GetFrameHeight() * 1.3f);
    bool startPending = appLoopbackStartPending_;
    if (startPending) {
        ImGui::BeginDisabled();
        ImGui::Button("Starting...", ctrlBtn);
        ImGui::EndDisabled();
    } else if (!appLoopbackStarted_) {
        if (ImGui::Button("Start", ctrlBtn)) {
            uint32_t pid = 0;
            if (!wa::parseApplicationLoopbackPid(appLoopbackPid_, pid)) {
                logLines_.push_back("application loopback error: invalid PID");
            } else {
                const wa::ProcessLoopbackMode mode = appLoopbackExclude_
                    ? wa::ProcessLoopbackMode::ExcludeTree
                    : wa::ProcessLoopbackMode::IncludeTree;
                logLines_.push_back(std::string("application loopback starting mode=") +
                                    wa::processLoopbackModeName(mode));
                beginApplicationLoopbackStart(pid, mode);
            }
        }
    } else {
        if (ImGui::Button("Stop", ctrlBtn)) {
            if (appLoopback_) appLoopback_->stop();
            appLoopbackStarted_ = false;
            resetVisuals(appLoopbackViz_);
            logLines_.push_back("application loopback stopped");
        }
    }

    ImGui::SeparatorText("Status");
    const char* ss[] = {"Idle", "Running", "Error"};
    ImGui::Text("overall=%s  cap=%s  sr=%u",
        ss[(int)appLoopbackMs_.overall], ss[(int)appLoopbackMs_.capState],
        appLoopbackMs_.sampleRate);
    // Running/pending: mode latched at Start; idle: preview from checkbox.
    const wa::ProcessLoopbackMode statusMode =
        freezeParams
            ? appLoopbackMode_
            : (appLoopbackExclude_ ? wa::ProcessLoopbackMode::ExcludeTree
                                   : wa::ProcessLoopbackMode::IncludeTree);
    ImGui::Text("mode=%s", wa::processLoopbackModeName(statusMode));
    ImGui::Text("xrun=%llu", (unsigned long long)appLoopbackMs_.capXruns);
    ImGui::Text("frames=%llu", (unsigned long long)appLoopbackMs_.capWrittenFrames);
    ImGui::Text("silent-packet=%llu  idle-fill=%llu",
        (unsigned long long)appLoopbackMs_.loopbackSilentPacketFrames,
        (unsigned long long)appLoopbackMs_.loopbackIdleSilenceFrames);
    ImGui::ProgressBar(appLoopbackMs_.capLevel, ImVec2(-1, 0), "level");
}

void AppUi::drawLeftPanel() {
    if (!monitorDevicesLoaded_) refreshMonitorDevices();

    // --- Devices ---
    ImGui::SeparatorText("Devices");
    if (ImGui::Button("Refresh devices")) refreshMonitorDevices();
    ImGui::SameLine();
    if (ImGui::Button("Options")) ImGui::OpenPopup("Audio parameters (advanced)");
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
            monitor_.stop();
            monitorStarted_ = false;
            resetVisuals(monitorViz_);
            logLines_.push_back("monitor stopped");
        }
    }
    ImGui::SameLine();
    // vertically center the playback checkbox against the taller Start/Stop button
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ctrlBtn.y - ImGui::GetFrameHeight()) * 0.5f);

    // Playback checkbox — always toggleable; applies live while running, else takes effect on next Start.
    if (ImGui::Checkbox("同步播放 (playback)", &playbackEnabled_)) {
        if (monitorStarted_) monitor_.setPlaybackEnabled(playbackEnabled_);
    }

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
    static const char* kCats[] = {"Other", "Communications", "Media", "Movie",
                                  "Game chat", "Speech", "Sound effects", "Game media"};

    ImGui::BeginDisabled(monitorStarted_);              // view-only while running

    ImGui::BeginGroup();                              // ---- Capture column
    ImGui::SeparatorText(wa::ui_text::kAdvancedCaptureSection);
    ImGui::PushID("capP");
    ImGui::PushItemWidth(190);
    bool capProps = capParams_.clientProperties.enabled;
    if (ImGui::Checkbox(wa::ui_text::kAdvancedSetClientProperties, &capProps))
        capParams_.clientProperties.enabled = capProps;
    ImGui::BeginDisabled(!capParams_.clientProperties.enabled);
    int v = (int)capParams_.clientProperties.category;
    if (ImGui::Combo("Category", &v, kCats, IM_ARRAYSIZE(kCats)))
        capParams_.clientProperties.category = (wa::AudioCategory)v;
    bool capOff = capParams_.clientProperties.offload;
    if (ImGui::Checkbox(wa::ui_text::kAdvancedHardwareOffload, &capOff))
        capParams_.clientProperties.offload = capOff;
    v = (int)capParams_.clientProperties.option;
    if (ImGui::Combo("Stream option", &v, wa::ui_text::kAdvancedStreamOptions,
                     wa::ui_text::kAdvancedStreamOptionCount))
        capParams_.clientProperties.option = (wa::StreamOption)v;
    ImGui::EndDisabled();
    v = (int)capParams_.bufferMs;
    if (ImGui::InputInt("Buffer (ms)", &v)) capParams_.bufferMs = (uint32_t)std::clamp(v, 0, 2000);
    if (ImGui::Button("Reset to system defaults")) capParams_ = wa::StreamParams{};
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
    v = (int)renParams_.clientProperties.category;
    if (ImGui::Combo("Category", &v, kCats, IM_ARRAYSIZE(kCats)))
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

void AppUi::drawChartsColumn(wa::MonitorEngine& engine, const wa::MonitorStatus& status,
                             VisualState& viz) {
    ensureRunningVisuals(status, viz, true);
    drawChartsFreezeToolbar(viz, status);

    for (int pos = 0; pos < (int)chartOrder_.size(); ++pos) {
        int id = chartOrder_[pos];
        ImGui::PushID(pos);
        const float bh = ImGui::GetFrameHeight();
        ImGui::Button("##drag", ImVec2(bh, bh));             // drag handle (move icon drawn on top)
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
        ImGui::SameLine(); ImGui::TextUnformatted(chartTitle(id));
        drawChartPanel(id, engine, status, viz);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("CHART_POS")) {
                int from = *(const int*)pl->Data;
                if (from != pos) {
                    int moved = chartOrder_[from];
                    chartOrder_.erase(chartOrder_.begin() + from);
                    chartOrder_.insert(chartOrder_.begin() + pos, moved);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    viz.resetYAxes = false; // one-shot Y restore consumed by all plots this frame
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
void AppUi::drawComboPanel(wa::MonitorEngine& engine, const wa::MonitorStatus& status,
                           VisualState& viz, bool renderSide) {
    const uint32_t sr         = status.sampleRate;
    const bool overallRunning = (status.overall     == wa::StreamState::Running && sr > 0);
    const bool renderRunning  = (status.renderState == wa::StreamState::Running);
    const bool refreshCharts  =
        wa::charts_freeze::shouldRefreshCharts(overallRunning, viz.chartsFrozen);
    const auto capturePlan = wa::capture_channel_view::makePlan(status.captureChannels);
    // Split layout follows the capture plan whenever the page is running (including frozen),
    // so multi-channel panels stay mounted while chart data is held.
    const bool splitCapture = !renderSide && overallRunning && capturePlan.split;

    if (splitCapture) {
        const uint32_t actualChannels = capturePlan.actualChannels;
        const uint32_t shownChannels = std::min<uint32_t>(
            capturePlan.visibleChannels, (uint32_t)viz.capChannelWaves.size());
        if (actualChannels > shownChannels)
            ImGui::Text("Capture channels: %u / %u channels shown", shownChannels, actualChannels);

        const uint64_t waveEnd = engine.capWritten();
        const size_t nn = (size_t)std::min<uint64_t>(waveEnd, (uint64_t)viz.waveN);
        for (uint32_t ch = 0; ch < shownChannels; ++ch) {
            std::vector<float>& wave = viz.capChannelWaves[(size_t)ch];
            bool ok = false;
            if (refreshCharts && viz.waveSr > 0 && nn > 0) {
                const int head = viz.waveN - (int)nn;
                std::fill(wave.begin(), wave.begin() + head, 0.f);
                ok = engine.snapshotCaptureChannelAt((uint16_t)ch, waveEnd, nn,
                                                     wave.data() + head);
            } else if (viz.chartsFrozen && viz.waveSr > 0 && viz.waveN > 0) {
                ok = true; // redraw last frozen channel buffer
            }
            const std::string title = "Capture Ch " + std::to_string(ch + 1u) + " waveform";
            ImGui::TextUnformatted(title.c_str());
            const std::string plotId = "##capWaveCh" + std::to_string(ch);
            drawWaveformPanel(viz, plotId.c_str(), wave.data(), viz.waveN, viz.waveSr, ok,
                              kMultiWaveH, kSlotCapWaveBase + (int)ch);
        }

        const uint32_t specChannels = std::min<uint32_t>(
            shownChannels,
            std::min<uint32_t>((uint32_t)viz.capChannelSpecs.size(),
                               (uint32_t)viz.capSpecWindows.size()));
        if (refreshCharts && viz.specSr > 0 && specChannels > 0) {
            const uint64_t written = engine.capWritten();
            wa::advanceAnalysis(written, viz.nextCapEnd, kFftWin, kFftHop, kCatchup, [&](uint64_t endIdx) {
                bool allGot = true;
                for (uint32_t ch = 0; ch < specChannels; ++ch) {
                    if (!engine.snapshotCaptureChannelAt((uint16_t)ch, endIdx, kFftWin,
                                                         viz.capSpecWindows[(size_t)ch].data())) {
                        allGot = false;
                        break;
                    }
                }
                if (!allGot) return;
                for (uint32_t ch = 0; ch < specChannels; ++ch) {
                    wa::magnitudeSpectrumDb(viz.capSpecWindows[(size_t)ch].data(), kFftWin,
                                            viz.workCap.data(), viz.magCap);
                    if (viz.capChannelSpecs[(size_t)ch])
                        viz.capChannelSpecs[(size_t)ch]->pushColumn(viz.magCap);
                }
            });
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

    // Snapshot the newest samples into the tail of the history buffer (progressive fill).
    // When charts are frozen, skip the snapshot and redraw the last buffers.
    std::vector<float>& wave = renderSide ? viz.renderWave : viz.capWave;
    bool ok = false;
    if (refreshCharts && viz.waveSr > 0 && (!renderSide || renderRunning)) {
        const size_t avail = (size_t)(renderSide ? engine.renderWritten() : engine.capWritten());
        const size_t nn = std::min(avail, (size_t)viz.waveN);
        if (nn > 0) {
            const int head = viz.waveN - (int)nn;
            std::fill(wave.begin(), wave.begin() + head, 0.f);
            uint64_t end = 0;
            ok = renderSide ? engine.snapshotRender(nn, wave.data() + head, end)
                            : engine.snapshotCapture(nn, wave.data() + head, end);
        }
    } else if (viz.chartsFrozen && viz.waveSr > 0 && viz.waveN > 0 &&
               (!renderSide || !viz.renderWave.empty())) {
        ok = true;
    }

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

    // Advance the FFT analysis feeding this stream's spectrogram (relocated here from the former
    // spectrum panels): one column per hop, catching up a few frames per poll. workCap_/workRender_
    // and magCap_/magRender_ are reused as scratch. Skipped while charts are frozen.
    if (refreshCharts && viz.specSr > 0 && (!renderSide || renderRunning)) {
        uint64_t se = 0;
        const uint64_t written = renderSide ? engine.renderWritten() : engine.capWritten();
        uint64_t& nextEnd      = renderSide ? viz.nextRenderEnd : viz.nextCapEnd;
        wa::advanceAnalysis(written, nextEnd, kFftWin, kFftHop, kCatchup, [&](uint64_t) {
            const bool got = renderSide ? engine.snapshotRender(kFftWin, viz.specWin.data(), se)
                                        : engine.snapshotCapture(kFftWin, viz.specWin.data(), se);
            if (!got) return;
            std::vector<std::complex<float>>& work = renderSide ? viz.workRender : viz.workCap;
            std::vector<float>&               mag  = renderSide ? viz.magRender  : viz.magCap;
            wa::magnitudeSpectrumDb(viz.specWin.data(), kFftWin, work.data(), mag);
            if (wa::Spectrogram* sp = renderSide ? viz.renderSpec.get() : viz.capSpec.get())
                sp->pushColumn(mag);
        });
    }

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

void AppUi::drawChartPanel(int id, wa::MonitorEngine& engine, const wa::MonitorStatus& status,
                           VisualState& viz) {
    switch (id) {
    case 0:   // Capture waveform + spectrogram combo
        drawComboPanel(engine, status, viz, false);
        break;
    case 1:   // Render waveform + spectrogram combo — data only while playback runs
        drawComboPanel(engine, status, viz, true);
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
