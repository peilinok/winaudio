#include "AppUi.h"
#include "imgui.h"
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

void AppUi::refreshDevices(wa::Engine& eng) {
    wa::DataFlow flow = (flowIdx_ == 0) ? wa::DataFlow::Capture : wa::DataFlow::Render;
    devices_ = eng.enumerate(flow);
    deviceIdx_ = 0;
    devicesLoaded_ = true;
}

void AppUi::draw(wa::Engine& eng) {
    if (!devicesLoaded_) refreshDevices(eng);

    ImGui::Begin("WinAudio");

    const char* backends[] = {"WASAPI-Shared"};
    ImGui::Combo("Backend", &backendIdx_, backends, 1);

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

    ImGui::InputText("WAV file", wavPath_, sizeof(wavPath_));

    wa::EngineStatus st = eng.poll();
    bool busy = (st.state == wa::EngineState::Capturing || st.state == wa::EngineState::Playing);

    if (!busy) {
        if (ImGui::Button("Start")) {
            wa::DeviceId id = devices_.empty() ? L"" : devices_[deviceIdx_].id;
            std::wstring path = utow(wavPath_);
            wa::Result r = (flowIdx_ == 0)
                ? eng.startCapture(wa::BackendKind::WasapiShared, id, path)
                : eng.startPlayback(wa::BackendKind::WasapiShared, id, path);
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
