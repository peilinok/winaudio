#include "AppUi.h"
#include "ComUtil.h"
#include "imgui.h"
#include "implot.h"
#include "Fft.h"
#include "Analysis.h"
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
constexpr float  kSpectrumH = 190.0f;  // spectrum
constexpr float  kComboH    = 400.0f;  // waveform+spectrogram combo cell content (split by comboRatio_)
constexpr float  kSplitH    = 6.0f;    // draggable splitter between waveform and spectrogram
// Plot slots for plotHovPrev_ (per-plot last-frame plot-area hover; see AppUi.h).
enum : int { kSlotCapWave, kSlotRenWave, kSlotCapSpectro, kSlotRenSpectro, kSlotCapSpectrum, kSlotRenSpectrum };

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
    capDevices_.clear();
    renderDevices_.clear();
    enumerator_.enumerate(wa::DataFlow::Capture, capDevices_);
    enumerator_.enumerate(wa::DataFlow::Render,  renderDevices_);
    capDevIdx_    = 0;
    renderDevIdx_ = 0;
    monitorDevicesLoaded_ = true;
}

void AppUi::stopAll() {
    monitor_.stop();
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
    case 2: return "Capture spectrum";
    case 3: return "Render spectrum";
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

    // Right column: six chart panels (fixed order in T2; T3 adds drag-reorder)
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

    ImGui::TextUnformatted("Capture device");
    if (ImGui::BeginListBox("##capdev", ImVec2(-1, 70))) {
        for (int i = 0; i < (int)capDevices_.size(); ++i) {
            std::string l = (capDevices_[i].isDefault ? "* " : "  ") + wtou(capDevices_[i].name);
            if (ImGui::Selectable((l + "##c" + std::to_string(i)).c_str(), capDevIdx_ == i))
                capDevIdx_ = i;
        }
        ImGui::EndListBox();
    }
    ImGui::TextUnformatted("Render device");
    if (ImGui::BeginListBox("##rendev", ImVec2(-1, 70))) {
        for (int i = 0; i < (int)renderDevices_.size(); ++i) {
            std::string l = (renderDevices_[i].isDefault ? "* " : "  ") + wtou(renderDevices_[i].name);
            if (ImGui::Selectable((l + "##r" + std::to_string(i)).c_str(), renderDevIdx_ == i))
                renderDevIdx_ = i;
        }
        ImGui::EndListBox();
    }

    // --- Control ---
    ImGui::SeparatorText("Control");
    const char* backends[] = {"WASAPI-Shared", "WASAPI-Exclusive"};
    ImGui::Combo("Backend", &backendIdx_, backends, 2);
    ImGui::SliderInt("Delay (ms)", &delayMs_, 0, 500);

    if (!monitorStarted_) {
        if (ImGui::Button("Start")) {
            wa::DeviceId capId = capDevices_.empty()    ? L"" : capDevices_[capDevIdx_].id;
            wa::DeviceId renId = renderDevices_.empty() ? L"" : renderDevices_[renderDevIdx_].id;
            wa::BackendKind kind = (backendIdx_ == 1) ? wa::BackendKind::WasapiExclusive
                                                      : wa::BackendKind::WasapiShared;
            wa::Result r = monitor_.start(kind, capId, renId, (uint32_t)delayMs_, playbackEnabled_);
            logLines_.push_back(r ? "monitor started" : ("monitor error: " + r.message));
            if (r) {
                monitorStarted_ = true;
                nextCapEnd_ = 0; nextRenderEnd_ = 0; specSr_ = 0; waveSr_ = 0;
            }
        }
    } else {
        if (ImGui::Button("Stop")) {
            monitor_.stop();
            monitorStarted_ = false;
            logLines_.push_back("monitor stopped");
        }
    }

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
            freqAxis_.resize(kFftWin / 2 + 1);
            for (size_t k = 0; k < freqAxis_.size(); ++k)
                freqAxis_[k] = (float)((double)k * (double)sr / (double)kFftWin);
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
    // so the panel is ALWAYS visible, consistent with the waveform/spectrum panels. The explicit
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

// Spectrum curve (log-frequency X, dBFS Y). Same X/Y zoom split as the time charts: wheel in the
// plot zooms frequency only; zoom dB via the Y ruler.
void AppUi::drawSpectrumPanel(const char* title, const std::vector<float>& mag, bool haveData, int slot) {
    if (!ImPlot::BeginPlot(title, ImVec2(-1, kSpectrumH))) return;
    const ImPlotAxisFlags yf = plotHovPrev_[slot] ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None;
    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None, yf);
    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
    ImPlot::SetupAxisLimits(ImAxis_X1, 20.0, (specSr_ > 0) ? (double)specSr_ / 2.0 : 24000.0, ImGuiCond_Once);
    ImPlot::SetupAxisLimits(ImAxis_Y1, -96.0, 0.0, ImGuiCond_Once);
    if (haveData && mag.size() > 1)
        ImPlot::PlotLine("##mag", freqAxis_.data() + 1, mag.data() + 1, (int)mag.size() - 1);
    drawYUnitLabel("dBFS");
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

    wa::Spectrogram* spec = nullptr;
    if (overallRunning) spec = renderSide ? (renderRunning ? renderSpec_.get() : nullptr) : capSpec_.get();
    const uint32_t hz = (sr > 0) ? sr : 48000u;   // idle: assume 48 kHz for the axis ranges
    drawSpectrogramPanel(renderSide ? "##renSpec" : "##capSpec", spec,
                         (double)(kSpecCols * kFftHop) / (double)hz, kComboH - waveH,
                         renderSide ? kSlotRenSpectro : kSlotCapSpectro);
}

void AppUi::drawChartPanel(int id) {
    const uint32_t sr         = ms_.sampleRate;
    const bool overallRunning = (ms_.overall    == wa::StreamState::Running && sr > 0);
    const bool renderRunning  = (ms_.renderState == wa::StreamState::Running);

    switch (id) {

    case 0:   // Capture waveform + spectrogram combo
        drawComboPanel(false);
        break;

    case 1:   // Render waveform + spectrogram combo — data only while playback runs
        drawComboPanel(true);
        break;

    case 2: { // Capture spectrum (also advances the analysis that feeds the capture spectrogram)
        if (overallRunning && specSr_ > 0) {
            uint64_t se = 0;
            wa::advanceAnalysis(monitor_.capWritten(), nextCapEnd_, kFftWin, kFftHop, kCatchup, [&](uint64_t) {
                if (monitor_.snapshotCapture(kFftWin, specWin_.data(), se)) {
                    wa::magnitudeSpectrumDb(specWin_.data(), kFftWin, workCap_.data(), magCap_);
                    if (capSpec_) capSpec_->pushColumn(magCap_);
                }
            });
        }
        drawSpectrumPanel("Capture spectrum", magCap_, overallRunning && specSr_ > 0, kSlotCapSpectrum);
        break;
    }

    case 3: { // Render spectrum — data only when renderRunning (also feeds the render spectrogram)
        if (overallRunning && specSr_ > 0 && renderRunning) {
            uint64_t se = 0;
            wa::advanceAnalysis(monitor_.renderWritten(), nextRenderEnd_, kFftWin, kFftHop, kCatchup, [&](uint64_t) {
                if (monitor_.snapshotRender(kFftWin, specWin_.data(), se)) {
                    wa::magnitudeSpectrumDb(specWin_.data(), kFftWin, workRender_.data(), magRender_);
                    if (renderSpec_) renderSpec_->pushColumn(magRender_);
                }
            });
        }
        drawSpectrumPanel("Render spectrum", magRender_, overallRunning && specSr_ > 0 && renderRunning, kSlotRenSpectrum);
        break;
    }

    default:
        break;
    }
}
