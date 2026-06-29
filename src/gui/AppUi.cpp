#include "AppUi.h"
#include "imgui.h"
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

static const int   kRates[] = {44100, 48000, 96000};
static const char* kRatesS[] = {"44100", "48000", "96000"};
static const int   kBits[]  = {16, 24, 32};
static const char* kBitsS[] = {"16", "24", "32"};
static const int   kChans[] = {1, 2};
static const char* kChansS[]= {"1", "2"};

void AppUi::refreshDevices(wa::Engine& eng) {
    wa::DataFlow flow = (flowIdx_ == 0) ? wa::DataFlow::Capture : wa::DataFlow::Render;
    devices_ = eng.enumerate(flow);
    deviceIdx_ = 0;
    devicesLoaded_ = true;
}

void AppUi::draw(wa::Engine& eng) {
    if (!devicesLoaded_) refreshDevices(eng);

    // Keep the panel wide enough that the format-controls row never clips.
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("WinAudio");

    const char* backends[] = {"WASAPI-Shared", "WASAPI-Exclusive"};
    ImGui::Combo("Backend", &backendIdx_, backends, 2);
    const bool exclusive = (backendIdx_ == 1);

    int prevFlow = flowIdx_;
    ImGui::RadioButton("Capture", &flowIdx_, 0); ImGui::SameLine();
    ImGui::RadioButton("Playback", &flowIdx_, 1);
    if (flowIdx_ != prevFlow) refreshDevices(eng);

    if (ImGui::Button("Refresh devices")) refreshDevices(eng);

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
        f.sampleRate = kRates[rateIdx_]; f.bitsPerSample = (uint16_t)kBits[bitsIdx_];
        f.channels = (uint16_t)kChans[chIdx_]; f.isFloat = isFloat_;
        wa::DeviceId id = devices_.empty() ? L"" : devices_[deviceIdx_].id;
        wa::DataFlow flow = (flowIdx_ == 0) ? wa::DataFlow::Capture : wa::DataFlow::Render;
        wa::Result pr = eng.probeFormat(wa::BackendKind::WasapiExclusive, flow, id, f);
        logLines_.push_back(std::string("probe ") + (pr ? "SUPPORTED" : "NOT SUPPORTED: " + pr.message));
    }
    if (!exclusive) ImGui::EndDisabled();

    ImGui::InputText("WAV file", wavPath_, sizeof(wavPath_));

    wa::EngineStatus st = eng.poll();
    bool busy = (st.state == wa::EngineState::Capturing || st.state == wa::EngineState::Playing);

    if (!busy) {
        if (ImGui::Button("Start")) {
            wa::DeviceId id = devices_.empty() ? L"" : devices_[deviceIdx_].id;
            std::wstring path = utow(wavPath_);
            wa::BackendKind kind = exclusive ? wa::BackendKind::WasapiExclusive
                                             : wa::BackendKind::WasapiShared;
            wa::AudioFormat f{};
            f.sampleRate = kRates[rateIdx_]; f.bitsPerSample = (uint16_t)kBits[bitsIdx_];
            f.channels = (uint16_t)kChans[chIdx_]; f.isFloat = isFloat_;
            const wa::AudioFormat* req = (exclusive && flowIdx_ == 0) ? &f : nullptr;
            wa::Result r = (flowIdx_ == 0)
                ? eng.startCapture(kind, id, path, req)
                : eng.startPlayback(kind, id, path, req);
            logLines_.push_back(r ? "started" : ("error: " + r.message));
        }
    } else {
        if (ImGui::Button("Stop")) { eng.stop(); logLines_.push_back("stopped"); }
    }

    ImGui::SameLine();
    const char* stateStr[] = {"Idle", "Capturing", "Playing", "Error"};
    ImGui::Text("State: %s  %.1fs", stateStr[(int)st.state], st.elapsedMs / 1000.f);

    ImGui::ProgressBar(st.levelL, ImVec2(-1, 0), "L");
    ImGui::ProgressBar(st.levelR, ImVec2(-1, 0), "R");
    ImGui::Text("overrun %llu  underrun %llu  fmt %u/%u/%u",
        (unsigned long long)st.overruns, (unsigned long long)st.underruns,
        st.actualFormat.sampleRate, st.actualFormat.bitsPerSample, st.actualFormat.channels);

    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 120), true);
    for (auto& l : logLines_) ImGui::TextUnformatted(l.c_str());
    ImGui::EndChild();

    ImGui::End();
}
