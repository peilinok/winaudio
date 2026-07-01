#include "AppUi.h"
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
static std::wstring utow(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

static const int   kRates[]  = {44100, 48000, 96000};
static const char* kRatesS[] = {"44100", "48000", "96000"};
static const int   kBits[]   = {16, 24, 32};
static const char* kBitsS[]  = {"16", "24", "32"};
static const int   kChans[]  = {1, 2};
static const char* kChansS[] = {"1", "2"};

void AppUi::refreshDevices() {
    wa::DataFlow flow = (modeIdx_ == 0) ? wa::DataFlow::Capture : wa::DataFlow::Render;
    devices_ = engine_.enumerate(flow);
    deviceIdx_ = 0;
    devicesLoaded_ = true;
}

void AppUi::refreshMonitorDevices() {
    capDevices_    = engine_.enumerate(wa::DataFlow::Capture);
    renderDevices_ = engine_.enumerate(wa::DataFlow::Render);
    capDevIdx_ = 0;
    renderDevIdx_ = 0;
    monitorDevicesLoaded_ = true;
}

void AppUi::stopAll() {
    engine_.stop();
    monitor_.stop();
}

void AppUi::draw() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("WinAudio");

    const char* modes[] = {"Capture", "Playback", "Monitor"};
    ImGui::Combo("Mode", &modeIdx_, modes, 3);
    if (modeIdx_ != prevMode_) {
        engine_.stop();
        monitor_.stop();
        monitorStarted_ = false;
        devicesLoaded_ = false;  // re-enumerate for the new flow
        prevMode_ = modeIdx_;
    }

    const char* backends[] = {"WASAPI-Shared", "WASAPI-Exclusive"};
    ImGui::Combo("Backend", &backendIdx_, backends, 2);
    const bool exclusive = (backendIdx_ == 1);

    if (modeIdx_ == 2) drawMonitor(exclusive);
    else               drawSingleStream(exclusive);

    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 120), true);
    for (auto& l : logLines_) ImGui::TextUnformatted(l.c_str());
    ImGui::EndChild();

    ImGui::End();
}

void AppUi::drawSingleStream(bool exclusive) {
    if (!devicesLoaded_) refreshDevices();

    if (ImGui::Button("Refresh devices")) refreshDevices();

    if (ImGui::BeginListBox("Devices")) {
        for (int i = 0; i < (int)devices_.size(); ++i) {
            std::string label = (devices_[i].isDefault ? "* " : "  ") + wtou(devices_[i].name);
            if (ImGui::Selectable(label.c_str(), deviceIdx_ == i)) deviceIdx_ = i;
        }
        ImGui::EndListBox();
    }

    if (!exclusive) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(76.0f);
    ImGui::Combo("Rate", &rateIdx_, kRatesS, IM_ARRAYSIZE(kRatesS)); ImGui::SameLine();
    ImGui::SetNextItemWidth(50.0f);
    ImGui::Combo("Bits", &bitsIdx_, kBitsS, IM_ARRAYSIZE(kBitsS)); ImGui::SameLine();
    ImGui::SetNextItemWidth(44.0f);
    ImGui::Combo("Ch", &chIdx_, kChansS, IM_ARRAYSIZE(kChansS)); ImGui::SameLine();
    ImGui::Checkbox("float", &isFloat_);
    if (ImGui::Button("Probe format")) {
        wa::AudioFormat f{};
        f.sampleRate    = kRates[rateIdx_];
        f.bitsPerSample = (uint16_t)kBits[bitsIdx_];
        f.channels      = (uint16_t)kChans[chIdx_];
        f.isFloat       = isFloat_;
        wa::DeviceId id   = devices_.empty() ? L"" : devices_[deviceIdx_].id;
        wa::DataFlow flow = (modeIdx_ == 0) ? wa::DataFlow::Capture : wa::DataFlow::Render;
        wa::Result pr     = engine_.probeFormat(wa::BackendKind::WasapiExclusive, flow, id, f);
        logLines_.push_back(std::string("probe ") + (pr ? "SUPPORTED" : "NOT SUPPORTED: " + pr.message));
    }
    if (!exclusive) ImGui::EndDisabled();

    ImGui::InputText("WAV file", wavPath_, sizeof(wavPath_));

    wa::EngineStatus st = engine_.poll();
    bool busy = (st.state == wa::EngineState::Capturing || st.state == wa::EngineState::Playing);

    if (!busy) {
        if (ImGui::Button("Start")) {
            wa::DeviceId id  = devices_.empty() ? L"" : devices_[deviceIdx_].id;
            std::wstring path = utow(wavPath_);
            wa::BackendKind kind = exclusive ? wa::BackendKind::WasapiExclusive
                                             : wa::BackendKind::WasapiShared;
            wa::AudioFormat f{};
            f.sampleRate    = kRates[rateIdx_];
            f.bitsPerSample = (uint16_t)kBits[bitsIdx_];
            f.channels      = (uint16_t)kChans[chIdx_];
            f.isFloat       = isFloat_;
            const wa::AudioFormat* req = (exclusive && modeIdx_ == 0) ? &f : nullptr;
            wa::Result r = (modeIdx_ == 0)
                ? engine_.startCapture(kind, id, path, req)
                : engine_.startPlayback(kind, id, path, req);
            logLines_.push_back(r ? "started" : ("error: " + r.message));
        }
    } else {
        if (ImGui::Button("Stop")) {
            engine_.stop();
            logLines_.push_back("stopped");
        }
    }

    ImGui::SameLine();
    const char* stateStr[] = {"Idle", "Capturing", "Playing", "Error"};
    ImGui::Text("State: %s  %.1fs", stateStr[(int)st.state], st.elapsedMs / 1000.f);

    ImGui::ProgressBar(st.levelL, ImVec2(-1, 0), "L");
    ImGui::ProgressBar(st.levelR, ImVec2(-1, 0), "R");
    ImGui::Text("overrun %llu  underrun %llu  fmt %u/%u/%u",
        (unsigned long long)st.overruns, (unsigned long long)st.underruns,
        st.actualFormat.sampleRate, st.actualFormat.bitsPerSample, st.actualFormat.channels);
}

void AppUi::drawMonitor(bool exclusive) {
    if (!monitorDevicesLoaded_) refreshMonitorDevices();
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
    ImGui::SliderInt("Delay (ms)", &delayMs_, 0, 500);

    if (!monitorStarted_) {
        if (ImGui::Button("Start")) {
            wa::DeviceId capId = capDevices_.empty()    ? L"" : capDevices_[capDevIdx_].id;
            wa::DeviceId renId = renderDevices_.empty() ? L"" : renderDevices_[renderDevIdx_].id;
            wa::BackendKind kind = exclusive ? wa::BackendKind::WasapiExclusive
                                            : wa::BackendKind::WasapiShared;
            wa::Result r = monitor_.start(kind, capId, renId, (uint32_t)delayMs_);
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

    wa::MonitorStatus ms = monitor_.poll();
    const char* ss[] = {"Idle", "Running", "Error"};
    ImGui::Text("overall=%s  cap=%s  ren=%s  sr=%u  delay=%ums",
        ss[(int)ms.overall], ss[(int)ms.capState], ss[(int)ms.renderState],
        ms.sampleRate, ms.delayMs);
    ImGui::Text("fifo=%.0fms  drift=%llu  xrun c/r=%llu/%llu",
        ms.fifoFillMs, (unsigned long long)ms.driftFixes,
        (unsigned long long)ms.capXruns, (unsigned long long)ms.renderXruns);
    ImGui::ProgressBar(ms.capLevel,    ImVec2(-1, 0), "cap");
    ImGui::ProgressBar(ms.renderLevel, ImVec2(-1, 0), "ren");

    // --- Time-domain waveforms (capture + delayed render), 50 ms window ---
    if (monitorStarted_ && ms.sampleRate > 0) {
        int n = (int)(0.05 * ms.sampleRate);           // 50 ms
        if (n < 1) n = 1;
        if (ms.sampleRate != waveSr_ || n != waveN_) { // rebuild only when rate/window changes
            waveSr_ = ms.sampleRate; waveN_ = n;
            capWave_.assign(n, 0.0f); renderWave_.assign(n, 0.0f);
            waveX_.resize(n);
            for (int i = 0; i < n; ++i) waveX_[i] = (float)i / (float)ms.sampleRate; // seconds
        }
        uint64_t endC = 0, endR = 0;
        bool okC = monitor_.snapshotCapture((size_t)n, capWave_.data(),    endC);
        bool okR = monitor_.snapshotRender ((size_t)n, renderWave_.data(), endR);

        if (ImPlot::BeginPlot("Capture waveform", ImVec2(-1, 120))) {
            ImPlot::SetupAxes("s", "amp", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImGuiCond_Always);
            if (okC) ImPlot::PlotLine("cap", waveX_.data(), capWave_.data(), n);
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Render waveform (delayed)", ImVec2(-1, 120))) {
            ImPlot::SetupAxes("s", "amp", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_None);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImGuiCond_Always);
            if (okR) ImPlot::PlotLine("ren", waveX_.data(), renderWave_.data(), n);
            ImPlot::EndPlot();
        }

        // --- Frequency-domain spectrum curves (2048 Hann FFT, dBFS, log-X) ---
        const size_t kWin = 2048, kHop = 512, kCatch = 8;
        const int kRows = 128, kCols = 200;
        if (ms.sampleRate != specSr_) {                 // rebuild scratch + x-axis on rate change only
            specSr_ = ms.sampleRate;
            workCap_.resize(kWin); workRender_.resize(kWin); specWin_.resize(kWin);
            freqAxis_.resize(kWin / 2 + 1);
            for (size_t k = 0; k < freqAxis_.size(); ++k)
                freqAxis_[k] = (float)((double)k * ms.sampleRate / kWin);   // bin k center freq (Hz)
            capSpec_    = std::make_unique<wa::Spectrogram>(kRows, kCols, 20.0, ms.sampleRate / 2.0, ms.sampleRate);
            renderSpec_ = std::make_unique<wa::Spectrogram>(kRows, kCols, 20.0, ms.sampleRate / 2.0, ms.sampleRate);
        }
        uint64_t se = 0;
        wa::advanceAnalysis(monitor_.capWritten(), nextCapEnd_, kWin, kHop, kCatch, [&](uint64_t){
            if (monitor_.snapshotCapture(kWin, specWin_.data(), se)) {
                wa::magnitudeSpectrumDb(specWin_.data(), kWin, workCap_.data(), magCap_);
                capSpec_->pushColumn(magCap_);
            }
        });
        wa::advanceAnalysis(monitor_.renderWritten(), nextRenderEnd_, kWin, kHop, kCatch, [&](uint64_t){
            if (monitor_.snapshotRender(kWin, specWin_.data(), se)) {
                wa::magnitudeSpectrumDb(specWin_.data(), kWin, workRender_.data(), magRender_);
                renderSpec_->pushColumn(magRender_);
            }
        });

        if (ImPlot::BeginPlot("Capture spectrum", ImVec2(-1, 140))) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
            ImPlot::SetupAxisLimits(ImAxis_X1, 20.0, ms.sampleRate / 2.0, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -96.0, 0.0, ImGuiCond_Always);
            if (magCap_.size() > 1)                       // skip bin 0 (DC) -- undefined on log-X
                ImPlot::PlotLine("cap", freqAxis_.data() + 1, magCap_.data() + 1, (int)magCap_.size() - 1);
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Render spectrum", ImVec2(-1, 140))) {
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
            ImPlot::SetupAxisLimits(ImAxis_X1, 20.0, ms.sampleRate / 2.0, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -96.0, 0.0, ImGuiCond_Always);
            if (magRender_.size() > 1)
                ImPlot::PlotLine("ren", freqAxis_.data() + 1, magRender_.data() + 1, (int)magRender_.size() - 1);
            ImPlot::EndPlot();
        }

        // --- Scrolling log-frequency spectrograms (heatmap, Viridis) ---
        const double histSec = (double)kCols * 512.0 / ms.sampleRate; // cols * hop / sr
        ImPlot::PushColormap(ImPlotColormap_Viridis);
        if (capSpec_ && ImPlot::BeginPlot("Capture spectrogram", ImVec2(-1, 160))) {
            ImPlot::PlotHeatmap("cap", capSpec_->data(), capSpec_->rows(), capSpec_->cols(),
                -96.0, 0.0, nullptr,
                ImPlotPoint(0, capSpec_->fmin()), ImPlotPoint(histSec, capSpec_->fmax()));
            ImPlot::EndPlot();
        }
        if (renderSpec_ && ImPlot::BeginPlot("Render spectrogram", ImVec2(-1, 160))) {
            ImPlot::PlotHeatmap("ren", renderSpec_->data(), renderSpec_->rows(), renderSpec_->cols(),
                -96.0, 0.0, nullptr,
                ImPlotPoint(0, renderSpec_->fmin()), ImPlotPoint(histSec, renderSpec_->fmax()));
            ImPlot::EndPlot();
        }
        ImPlot::PopColormap();
    }
}
