#include "AppUi.h"
#include "ComUtil.h"
#include "imgui.h"
#include "implot.h"
#include "Fft.h"
#include "Analysis.h"
#include <cfloat>
#include <string>

static std::string wtou(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
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

const char* AppUi::chartTitle(int id) {
    switch (id) {
    case 0: return "Capture waveform";
    case 1: return "Render waveform (delayed)";
    case 2: return "Capture spectrum";
    case 3: return "Render spectrum";
    case 4: return "Capture spectrogram";
    case 5: return "Render spectrogram";
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

    if (overallRunning) {
        // Waveform buffers: rebuild when rate or 50-ms window size changes.
        const int n = (int)(0.05 * (double)sr);
        const int waveN = n > 0 ? n : 1;
        if (sr != waveSr_ || waveN != waveN_) {
            waveSr_ = sr; waveN_ = waveN;
            capWave_.assign((size_t)waveN_, 0.f);
            renderWave_.assign((size_t)waveN_, 0.f);
            waveX_.resize((size_t)waveN_);
            for (int i = 0; i < waveN_; ++i)
                waveX_[(size_t)i] = (float)i / (float)sr;
        }

        // Spectrum / spectrogram buffers: rebuild on rate change; recreate renderSpec_ if cleared.
        const size_t kWin  = 2048;
        const int    kRows = 128, kCols = 200;
        if (sr != specSr_) {
            specSr_ = sr;
            workCap_.resize(kWin); workRender_.resize(kWin); specWin_.resize(kWin);
            freqAxis_.resize(kWin / 2 + 1);
            for (size_t k = 0; k < freqAxis_.size(); ++k)
                freqAxis_[k] = (float)((double)k * (double)sr / (double)kWin);
            capSpec_    = std::make_unique<wa::Spectrogram>(kRows, kCols, 20.0, (double)sr / 2.0, sr);
            renderSpec_ = std::make_unique<wa::Spectrogram>(kRows, kCols, 20.0, (double)sr / 2.0, sr);
        } else if (!renderSpec_) {
            // renderSpec_ was cleared by a playback-stop transition; recreate fresh.
            renderSpec_ = std::make_unique<wa::Spectrogram>(kRows, kCols, 20.0, (double)specSr_ / 2.0, specSr_);
        }
    }

    for (int pos = 0; pos < (int)chartOrder_.size(); ++pos) {
        int id = chartOrder_[pos];
        ImGui::PushID(pos);
        ImGui::Button("\xe2\x98\xb0");                       // drag handle (☰ U+2630)
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

void AppUi::drawChartPanel(int id) {
    const uint32_t sr         = ms_.sampleRate;
    const bool overallRunning = (ms_.overall    == wa::StreamState::Running && sr > 0);
    const bool renderRunning  = (ms_.renderState == wa::StreamState::Running);

    switch (id) {

    case 0: { // Capture waveform
        if (overallRunning && waveSr_ > 0) {
            uint64_t endC = 0;
            const bool okC = monitor_.snapshotCapture((size_t)waveN_, capWave_.data(), endC);
            if (ImPlot::BeginPlot("Capture waveform", ImVec2(-1, 120))) {
                ImPlot::SetupAxes("s", "amp", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImGuiCond_Always);
                if (okC) ImPlot::PlotLine("cap", waveX_.data(), capWave_.data(), waveN_);
                ImPlot::EndPlot();
            }
        } else {
            if (ImPlot::BeginPlot("Capture waveform", ImVec2(-1, 120))) {
                ImPlot::SetupAxes("s", "amp", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImGuiCond_Always);
                ImPlot::EndPlot();
            }
        }
        break;
    }

    case 1: { // Render waveform — data only when renderRunning
        if (overallRunning && waveSr_ > 0) {
            bool okR = false;
            if (renderRunning) {
                uint64_t endR = 0;
                okR = monitor_.snapshotRender((size_t)waveN_, renderWave_.data(), endR);
            }
            if (ImPlot::BeginPlot("Render waveform (delayed)", ImVec2(-1, 120))) {
                ImPlot::SetupAxes("s", "amp", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImGuiCond_Always);
                if (okR) ImPlot::PlotLine("ren", waveX_.data(), renderWave_.data(), waveN_);
                ImPlot::EndPlot();
            }
        } else {
            if (ImPlot::BeginPlot("Render waveform (delayed)", ImVec2(-1, 120))) {
                ImPlot::SetupAxes("s", "amp", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImGuiCond_Always);
                ImPlot::EndPlot();
            }
        }
        break;
    }

    case 2: { // Capture spectrum
        if (overallRunning && specSr_ > 0) {
            const size_t kWin = 2048, kHop = 512, kCatch = 8;
            uint64_t se = 0;
            wa::advanceAnalysis(monitor_.capWritten(), nextCapEnd_, kWin, kHop, kCatch, [&](uint64_t) {
                if (monitor_.snapshotCapture(kWin, specWin_.data(), se)) {
                    wa::magnitudeSpectrumDb(specWin_.data(), kWin, workCap_.data(), magCap_);
                    if (capSpec_) capSpec_->pushColumn(magCap_);
                }
            });
            if (ImPlot::BeginPlot("Capture spectrum", ImVec2(-1, 140))) {
                ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                ImPlot::SetupAxisLimits(ImAxis_X1, 20.0, (double)sr / 2.0, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -96.0, 0.0, ImGuiCond_Always);
                if (magCap_.size() > 1)
                    ImPlot::PlotLine("cap", freqAxis_.data() + 1, magCap_.data() + 1, (int)magCap_.size() - 1);
                ImPlot::EndPlot();
            }
        } else {
            if (ImPlot::BeginPlot("Capture spectrum", ImVec2(-1, 140))) {
                ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                ImPlot::SetupAxisLimits(ImAxis_X1, 20.0, 24000.0, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -96.0, 0.0, ImGuiCond_Always);
                ImPlot::EndPlot();
            }
        }
        break;
    }

    case 3: { // Render spectrum — data only when renderRunning
        if (overallRunning && specSr_ > 0) {
            if (renderRunning) {
                const size_t kWin = 2048, kHop = 512, kCatch = 8;
                uint64_t se = 0;
                wa::advanceAnalysis(monitor_.renderWritten(), nextRenderEnd_, kWin, kHop, kCatch, [&](uint64_t) {
                    if (monitor_.snapshotRender(kWin, specWin_.data(), se)) {
                        wa::magnitudeSpectrumDb(specWin_.data(), kWin, workRender_.data(), magRender_);
                        if (renderSpec_) renderSpec_->pushColumn(magRender_);
                    }
                });
            }
            if (ImPlot::BeginPlot("Render spectrum", ImVec2(-1, 140))) {
                ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                ImPlot::SetupAxisLimits(ImAxis_X1, 20.0, (double)sr / 2.0, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -96.0, 0.0, ImGuiCond_Always);
                if (renderRunning && magRender_.size() > 1)
                    ImPlot::PlotLine("ren", freqAxis_.data() + 1, magRender_.data() + 1, (int)magRender_.size() - 1);
                ImPlot::EndPlot();
            }
        } else {
            if (ImPlot::BeginPlot("Render spectrum", ImVec2(-1, 140))) {
                ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
                ImPlot::SetupAxisLimits(ImAxis_X1, 20.0, 24000.0, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -96.0, 0.0, ImGuiCond_Always);
                ImPlot::EndPlot();
            }
        }
        break;
    }

    case 4: { // Capture spectrogram
        if (overallRunning && capSpec_) {
            const double histSec = 200.0 * 512.0 / (double)sr; // kCols * kHop / sr
            ImPlot::PushColormap(ImPlotColormap_Viridis);
            if (ImPlot::BeginPlot("Capture spectrogram", ImVec2(-1, 160))) {
                ImPlot::PlotHeatmap("cap", capSpec_->data(), capSpec_->rows(), capSpec_->cols(),
                    -96.0, 0.0, nullptr,
                    ImPlotPoint(0, capSpec_->fmin()), ImPlotPoint(histSec, capSpec_->fmax()));
                ImPlot::EndPlot();
            }
            ImPlot::PopColormap();
        } else {
            if (ImPlot::BeginPlot("Capture spectrogram", ImVec2(-1, 160))) {
                ImPlot::EndPlot();
            }
        }
        break;
    }

    case 5: { // Render spectrogram — data only when renderRunning
        if (overallRunning && renderRunning && renderSpec_) {
            const double histSec = 200.0 * 512.0 / (double)sr;
            ImPlot::PushColormap(ImPlotColormap_Viridis);
            if (ImPlot::BeginPlot("Render spectrogram", ImVec2(-1, 160))) {
                ImPlot::PlotHeatmap("ren", renderSpec_->data(), renderSpec_->rows(), renderSpec_->cols(),
                    -96.0, 0.0, nullptr,
                    ImPlotPoint(0, renderSpec_->fmin()), ImPlotPoint(histSec, renderSpec_->fmax()));
                ImPlot::EndPlot();
            }
            ImPlot::PopColormap();
        } else {
            if (ImPlot::BeginPlot("Render spectrogram", ImVec2(-1, 160))) {
                ImPlot::EndPlot();
            }
        }
        break;
    }

    default:
        break;
    }
}
