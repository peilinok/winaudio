#pragma once
#include <string>
#include <vector>
#include "Engine.h"

class AppUi {
public:
    void draw(wa::Engine& eng);
private:
    void refreshDevices(wa::Engine& eng);

    int  backendIdx_ = 0;          // 0 = WASAPI-Shared (only MVP option)
    int  flowIdx_ = 0;             // 0 = capture, 1 = playback
    int  deviceIdx_ = 0;
    bool devicesLoaded_ = false;
    std::vector<wa::DeviceInfo> devices_;
    char wavPath_[260] = "cap.wav";
    std::vector<std::string> logLines_;
};
