#pragma once
#include <string>
#include <vector>
#include "Engine.h"

class AppUi {
public:
    void draw(wa::Engine& eng);
private:
    void refreshDevices(wa::Engine& eng);

    int  backendIdx_ = 0;          // 0 = WASAPI-Shared, 1 = WASAPI-Exclusive
    int  flowIdx_ = 0;             // 0 = capture, 1 = playback
    int  deviceIdx_ = 0;
    bool devicesLoaded_ = false;
    std::vector<wa::DeviceInfo> devices_;
    char wavPath_[260] = "cap.wav";
    std::vector<std::string> logLines_;
    int  rateIdx_ = 0;   // index into kRates
    int  bitsIdx_ = 0;   // index into kBits
    int  chIdx_ = 1;     // index into kChannels (default 2ch)
    bool isFloat_ = false;
};
