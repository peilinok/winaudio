#include "AppUi.h"
#include "ComUtil.h"
#include "imgui.h"
#include "implot.h"
#include "Fft.h"
#include "Analysis.h"
#include "FormatSpec.h"
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
// Waveform shares the spectrogram's time window: kSpecCols*kFftHop samples (see drawChartsColumn).
// Chart panel heights (px).
constexpr float  kComboH    = 400.0f;  // waveform+spectrogram combo cell content (split by comboRatio_)
constexpr float  kSplitH    = 6.0f;    // draggable splitter between waveform and spectrogram
// Plot slots for plotHovPrev_ (per-plot last-frame plot-area hover; see AppUi.h).
enum : int { kSlotCapWave, kSlotRenWave, kSlotCapSpectro, kSlotRenSpectro };

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
    monitorDevicesLoaded_ = true;
}

void AppUi::stopAll() {
    monitor_.stop();
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

    // Safety clamp (handles backend-switch where old index may be out of range).
    // Layout: 0=System default, 1..nOk=ok candidates, nOk+1=Custom...
    if (fmtChoiceIdx_ < 0 || fmtChoiceIdx_ > nOk + 1) fmtChoiceIdx_ = 0;

    // Combo preview
    auto fmtStr = [](const wa::AudioFormat& fmt) -> std::string {
        std::string s = std::to_string(fmt.sampleRate) + "/" +
                        std::to_string(fmt.bitsPerSample) + "/" +
                        std::to_string(fmt.channels);
        if (fmt.isFloat) s += "f";
        return s;
    };
    const std::string preview = (fmtChoiceIdx_ == 0)    ? "System default"
                              : (fmtChoiceIdx_ <= nOk)  ? fmtStr(okFmts[(size_t)(fmtChoiceIdx_ - 1)])
                              : "Custom...";
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
        if (ImGui::Selectable("Custom...", fmtChoiceIdx_ == nOk + 1))
            fmtChoiceIdx_ = nOk + 1;
        ImGui::EndCombo();
    }

    // Custom format input (shown when "Custom..." is selected)
    if (fmtChoiceIdx_ == nOk + 1) {
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("##fmtInput", fmtCustom_, sizeof(fmtCustom_));
        ImGui::SameLine();
        if (ImGui::Button("Apply")) {
            wa::AudioFormat parsed{};
            if (wa::parseFormatSpec(std::string(fmtCustom_), parsed)) {
                selectedFmt_ = parsed;
                haveFmt_     = true;
            } else {
                logLines_.push_back("invalid format");
            }
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

void AppUi::draw() {
    // Poll once; detect renderState Running->non-Running to clear stale playback chart data.
    ms_ = monitor_.poll();
    const int curRenderState = (int)ms_.renderState;
    if (prevRenderState_ == (int)wa::StreamState::Running &&
        curRenderState  != (int)wa::StreamState::Running) {
        magRender_.clear();
        renderSpec_.reset();
        nextRenderEnd_ = 0;
        std::fill(renderWave_.begin(), renderWave_.end(), 0.f);
    }
    prevRenderState_ = curRenderState;

    ImGui::SetNextWindowSizeConstraints(ImVec2(800, 400), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("WinAudio");

    // Left column: Devices / Control / Status / Log
    ImGui::BeginChild("left", ImVec2(360, 0), true);
    drawLeftPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    // Right column: two combo panels (capture + render), drag to reorder
    ImGui::BeginChild("charts", ImVec2(0, 0), true);
    drawChartsColumn();
    ImGui::EndChild();

    ImGui::End();
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
                nextCapEnd_ = 0; nextRenderEnd_ = 0; specSr_ = 0; waveSr_ = 0;
            }
        }
    } else {
        if (ImGui::Button("Stop", ctrlBtn)) {
            monitor_.stop();
            monitorStarted_ = false;
            logLines_.push_back("monitor stopped");
        }
    }
    ImGui::SameLine();
    // vertically center the playback checkbox against the taller Start/Stop button
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ctrlBtn.y - ImGui::GetFrameHeight()) * 0.5f);

    // Playback checkbox — disabled until monitor is started
    if (!monitorStarted_) ImGui::BeginDisabled();
    if (ImGui::Checkbox("同步播放 (playback)", &playbackEnabled_))
        monitor_.setPlaybackEnabled(playbackEnabled_);
    if (!monitorStarted_) ImGui::EndDisabled();

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
    ImGui::ProgressBar(ms_.capLevel,    ImVec2(-1, 0), "cap");
    ImGui::ProgressBar(ms_.renderLevel, ImVec2(-1, 0), "ren");

    // --- Log (fills remaining height) ---
    ImGui::SeparatorText("Log");
    ImGui::BeginChild("log", ImVec2(0, 0), true);
    for (const auto& l : logLines_) ImGui::TextUnformatted(l.c_str());
    ImGui::EndChild();
}

void AppUi::drawAdvancedModal() {
    if (!ImGui::BeginPopupModal("Audio parameters (advanced)", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("All values default to system-recommended (no override is injected unless "
                       "you change them). Category/option/offload/ducking require WASAPI-Shared; "
                       "only Buffer applies to Exclusive. Offload and ducking are render-only "
                       "(WASAPI has no capture offload).");
    if (monitorStarted_)
        ImGui::TextColored(ImVec4(1.00f, 0.80f, 0.30f, 1.00f),
                           "Running: parameters are read-only. Stop the monitor to change them.");
    ImGui::Separator();
    static const char* kCats[] = {"System default", "Other", "Communications", "Media", "Movie",
                                  "Game chat", "Speech", "Sound effects", "Game media"};
    static const char* kOpts[] = {"System default", "Raw (bypass APO)", "Match format"};

    ImGui::BeginDisabled(monitorStarted_);              // view-only while running

    ImGui::BeginGroup();                              // ---- Capture column
    ImGui::SeparatorText("Capture");
    ImGui::PushID("capP");
    ImGui::PushItemWidth(190);
    int v = (int)capParams_.category;
    if (ImGui::Combo("Category", &v, kCats, 9)) capParams_.category = (wa::AudioCategory)v;
    v = (int)capParams_.option;
    if (ImGui::Combo("Stream option", &v, kOpts, 3)) capParams_.option = (wa::StreamOption)v;
    v = (int)capParams_.bufferMs;
    if (ImGui::InputInt("Buffer (ms)", &v)) capParams_.bufferMs = (uint32_t)std::clamp(v, 0, 2000);
    if (ImGui::Button("Reset to system defaults")) capParams_ = wa::StreamParams{};
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::EndGroup();

    ImGui::SameLine(0, 24);

    ImGui::BeginGroup();                              // ---- Render column
    ImGui::SeparatorText("Render");
    ImGui::PushID("renP");
    ImGui::PushItemWidth(190);
    v = (int)renParams_.category;
    if (ImGui::Combo("Category", &v, kCats, 9)) renParams_.category = (wa::AudioCategory)v;
    v = (int)renParams_.option;
    if (ImGui::Combo("Stream option", &v, kOpts, 3)) renParams_.option = (wa::StreamOption)v;
    bool off = renParams_.offload == wa::OffloadMode::Force;
    if (ImGui::Checkbox("Hardware offload", &off)) renParams_.offload = off ? wa::OffloadMode::Force : wa::OffloadMode::Default;
    bool duck = renParams_.ducking == wa::DuckingMode::OptOut;
    if (ImGui::Checkbox("Ducking opt-out", &duck)) renParams_.ducking = duck ? wa::DuckingMode::OptOut : wa::DuckingMode::Default;
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

void AppUi::drawChartsColumn() {
    const uint32_t sr         = ms_.sampleRate;
    const bool overallRunning = (ms_.overall == wa::StreamState::Running && sr > 0);

    // Shared time axis (seconds) for all time-domain charts = the spectrogram's history window.
    // Initialize once; reset to the full window whenever the rate changes (below).
    const uint32_t hz = (sr > 0) ? sr : 48000u;
    if (xLink1_ <= 0.0) { xLink0_ = 0.0; xLink1_ = (double)(kSpecCols * kFftHop) / (double)hz; }

    if (overallRunning) {
        // Waveform buffers cover the same window as the spectrogram (kSpecCols*kFftHop samples).
        if (sr != waveSr_) {
            waveSr_ = sr; waveN_ = (int)(kSpecCols * kFftHop);
            capWave_.assign((size_t)waveN_, 0.f);
            renderWave_.assign((size_t)waveN_, 0.f);
            xLink0_ = 0.0; xLink1_ = (double)(kSpecCols * kFftHop) / (double)sr;  // reset zoom to full window
        }

        // Spectrum / spectrogram buffers: rebuild on rate change; recreate renderSpec_ if cleared.
        if (sr != specSr_) {
            specSr_ = sr;
            workCap_.resize(kFftWin); workRender_.resize(kFftWin); specWin_.resize(kFftWin);
            capSpec_    = std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0, (double)sr / 2.0, sr);
            renderSpec_ = std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0, (double)sr / 2.0, sr);
        } else if (!renderSpec_) {
            // renderSpec_ was cleared by a playback-stop transition; recreate fresh.
            renderSpec_ = std::make_unique<wa::Spectrogram>(kSpecRows, kSpecCols, 20.0, (double)specSr_ / 2.0, specSr_);
        }
    }

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
        drawChartPanel(id);
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
void AppUi::drawSpectrogramPanel(const char* plotId, wa::Spectrogram* spec, double histSec, float height, int slot) {
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
        const ImPlotAxisFlags yf = (plotHovPrev_[slot] ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None)
                                 | ImPlotAxisFlags_Opposite;
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels, yf);
        ImPlot::SetupAxisLinks(ImAxis_X1, &xLink0_, &xLink1_);   // shared time axis (waveforms + spectrograms)
        // Clamp panning/zooming to the buffered history / frequency range — no empty space.
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, histSec);
        ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, loL, hiL);
        ImPlot::SetupAxisLimits(ImAxis_Y1, lo0, hiL, ImGuiCond_Once);
        if (nTick > 0) ImPlot::SetupAxisTicks(ImAxis_Y1, tickV, nTick, tickL);
        if (spec)
            ImPlot::PlotHeatmap("##hm", spec->data(), spec->rows(), spec->cols(), -96.0, 0.0, nullptr,
                ImPlotPoint(0, loL), ImPlotPoint(histSec, hiL));
        drawYUnitLabel("Hz", true);
        plotHovPrev_[slot] = ImPlot::IsPlotHovered();
        ImPlot::EndPlot();
    }
    ImPlot::PopColormap();
}

// Draw one waveform over the shared time axis (xLink0_..xLink1_ seconds). wave[i] is at t = i/sr,
// so the buffer's tail holds the newest samples (right edge). Level-of-detail: zoomed in -> raw
// line; zoomed out -> per-pixel min/max envelope, so detail sharpens as you zoom and the waveform
// stays column-aligned with the spectrogram.
void AppUi::drawWaveformPanel(const char* plotId, const float* wave, int n, uint32_t sr, bool haveData, float height, int slot) {
    if (!ImPlot::BeginPlot(plotId, ImVec2(-1, height))) return;
    // Y locked while the plot area was hovered last frame -> in-plot wheel/drag act on X only;
    // zoom amplitude via the Y ruler. X tick labels hidden: the spectrogram below shows the time.
    // Audition-style: time ruler on TOP of the waveform, dB ruler on the RIGHT.
    const ImPlotAxisFlags yf = (plotHovPrev_[slot] ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None)
                             | ImPlotAxisFlags_Opposite;
    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_Opposite, yf);
    ImPlot::SetupAxisLinks(ImAxis_X1, &xLink0_, &xLink1_);        // shared time axis
    // Clamp panning/zooming to the buffered history — no scrolling into empty space.
    const double hist = (sr > 0 && n > 0) ? (double)n / (double)sr
                                          : (double)(kSpecCols * kFftHop) / 48000.0;
    ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0.0, hist);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, -1.0, 1.0);
    ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImGuiCond_Once);
    // dB ruler for the warped scale (positions = dbWarp of the label, with kWaveDbFloor = -60).
    static const double kAmpV[] = {-1.0, -0.9, -0.8, -0.6, -0.2, 0.0, 0.2, 0.6, 0.8, 0.9, 1.0};
    static const char*  kAmpL[] = {"0", "-6", "-12", "-24", "-48", "-inf", "-48", "-24", "-12", "-6", "0"};
    ImPlot::SetupAxisTicks(ImAxis_Y1, kAmpV, 11, kAmpL);
    if (haveData && n > 0 && sr > 0) {
        const double invSr = 1.0 / (double)sr;
        int iLo = (int)std::floor(xLink0_ * (double)sr);
        int iHi = (int)std::ceil (xLink1_ * (double)sr);
        if (iLo < 0) iLo = 0;
        if (iHi > n - 1) iHi = n - 1;
        if (iHi > iLo) {
            const int visN = iHi - iLo + 1;
            float pw = ImPlot::GetPlotSize().x;
            if (pw < 1.0f) pw = 1.0f;
            if (visN <= (int)(2.0f * pw)) {
                envMax_.resize((size_t)visN);                     // scratch: warped raw samples
                for (int i = 0; i < visN; ++i) envMax_[(size_t)i] = dbWarp(wave[iLo + i]);
                ImPlot::SetNextLineStyle(ImVec4(0.40f, 0.89f, 0.59f, 1.00f));   // Audition green
                ImPlot::PlotLine("##wave", envMax_.data(), visN, invSr, (double)iLo * invSr);
            } else {
                const int cols = (int)pw;
                envX_.resize((size_t)cols); envMin_.resize((size_t)cols); envMax_.resize((size_t)cols);
                for (int c = 0; c < cols; ++c) {
                    int s0 = iLo + (int)((int64_t)visN * c / cols);
                    int s1 = iLo + (int)((int64_t)visN * (c + 1) / cols);
                    if (s1 <= s0) s1 = s0 + 1;
                    if (s1 > n)   s1 = n;
                    float mn = wave[s0], mx = wave[s0];
                    for (int s = s0 + 1; s < s1; ++s) { mn = std::min(mn, wave[s]); mx = std::max(mx, wave[s]); }
                    envX_[(size_t)c]   = (float)(0.5 * (double)(s0 + s1) * invSr);
                    envMin_[(size_t)c] = dbWarp(mn);              // warp commutes with min/max
                    envMax_[(size_t)c] = dbWarp(mx);
                }
                ImPlot::SetNextFillStyle(ImVec4(0.40f, 0.89f, 0.59f, 1.00f), 1.0f);   // Audition green, solid
                ImPlot::PlotShaded("##wave", envX_.data(), envMin_.data(), envMax_.data(), cols);
            }
        }
    }
    drawYUnitLabel("dB", true);
    plotHovPrev_[slot] = ImPlot::IsPlotHovered();
    ImPlot::EndPlot();
}

// One combined cell per stream: waveform on top, spectrogram below (Audition-style), sharing the
// linked time axis, with a draggable horizontal splitter between them. comboRatio_ is shared by
// both streams so the capture and render cells stay height-aligned for side-by-side comparison.
void AppUi::drawComboPanel(bool renderSide) {
    const uint32_t sr         = ms_.sampleRate;
    const bool overallRunning = (ms_.overall     == wa::StreamState::Running && sr > 0);
    const bool renderRunning  = (ms_.renderState == wa::StreamState::Running);

    // Snapshot the newest samples into the tail of the history buffer (progressive fill).
    std::vector<float>& wave = renderSide ? renderWave_ : capWave_;
    bool ok = false;
    if (overallRunning && waveSr_ > 0 && (!renderSide || renderRunning)) {
        const size_t avail = (size_t)(renderSide ? monitor_.renderWritten() : monitor_.capWritten());
        const size_t nn = std::min(avail, (size_t)waveN_);
        if (nn > 0) {
            const int head = waveN_ - (int)nn;
            std::fill(wave.begin(), wave.begin() + head, 0.f);
            uint64_t end = 0;
            ok = renderSide ? monitor_.snapshotRender(nn, wave.data() + head, end)
                            : monitor_.snapshotCapture(nn, wave.data() + head, end);
        }
    }

    const float waveH = kComboH * comboRatio_;
    drawWaveformPanel(renderSide ? "##renWave" : "##capWave", wave.data(), waveN_, waveSr_, ok,
                      waveH, renderSide ? kSlotRenWave : kSlotCapWave);

    // Splitter: drag to rebalance waveform vs spectrogram height (the axes re-lay out to fit).
    ImGui::InvisibleButton(renderSide ? "##renSplit" : "##capSplit",
                           ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f), kSplitH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive())
        comboRatio_ = std::clamp(comboRatio_ + ImGui::GetIO().MouseDelta.y / kComboH, 0.15f, 0.85f);
    const ImVec2 smn = ImGui::GetItemRectMin(), smx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(smn.x, (smn.y + smx.y) * 0.5f),
                                        ImVec2(smx.x, (smn.y + smx.y) * 0.5f),
                                        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);

    // Advance the FFT analysis feeding this stream's spectrogram (relocated here from the former
    // spectrum panels): one column per hop, catching up a few frames per poll. workCap_/workRender_
    // and magCap_/magRender_ are reused as scratch.
    if (overallRunning && specSr_ > 0 && (!renderSide || renderRunning)) {
        uint64_t se = 0;
        const uint64_t written = renderSide ? monitor_.renderWritten() : monitor_.capWritten();
        uint64_t& nextEnd      = renderSide ? nextRenderEnd_ : nextCapEnd_;
        wa::advanceAnalysis(written, nextEnd, kFftWin, kFftHop, kCatchup, [&](uint64_t) {
            const bool got = renderSide ? monitor_.snapshotRender(kFftWin, specWin_.data(), se)
                                        : monitor_.snapshotCapture(kFftWin, specWin_.data(), se);
            if (!got) return;
            std::vector<std::complex<float>>& work = renderSide ? workRender_ : workCap_;
            std::vector<float>&               mag  = renderSide ? magRender_  : magCap_;
            wa::magnitudeSpectrumDb(specWin_.data(), kFftWin, work.data(), mag);
            if (wa::Spectrogram* sp = renderSide ? renderSpec_.get() : capSpec_.get())
                sp->pushColumn(mag);
        });
    }

    wa::Spectrogram* spec = nullptr;
    if (overallRunning) spec = renderSide ? (renderRunning ? renderSpec_.get() : nullptr) : capSpec_.get();
    const uint32_t hz = (sr > 0) ? sr : 48000u;   // idle: assume 48 kHz for the axis ranges
    drawSpectrogramPanel(renderSide ? "##renSpec" : "##capSpec", spec,
                         (double)(kSpecCols * kFftHop) / (double)hz, kComboH - waveH,
                         renderSide ? kSlotRenSpectro : kSlotCapSpectro);
}

void AppUi::drawChartPanel(int id) {
    switch (id) {
    case 0:   // Capture waveform + spectrogram combo
        drawComboPanel(false);
        break;
    case 1:   // Render waveform + spectrogram combo — data only while playback runs
        drawComboPanel(true);
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
