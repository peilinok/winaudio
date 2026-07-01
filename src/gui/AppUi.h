#pragma once
#include <string>
#include <vector>
#include "Engine.h"
#include "MonitorEngine.h"

class AppUi {
public:
    void draw();          // AppUi owns the engines now (no argument)
    void stopAll();       // stop both engines on shutdown (idempotent)
private:
    void refreshDevices();         // single-stream list (mode Capture/Playback)
    void refreshMonitorDevices();  // capture + render lists (mode Monitor)
    void drawSingleStream(bool exclusive);  // existing Capture/Playback UI body
    void drawMonitor(bool exclusive);       // new Monitor panel

    wa::Engine        engine_;
    wa::MonitorEngine monitor_;

    int  modeIdx_  = 0;   // 0=Capture, 1=Playback, 2=Monitor  (replaces flowIdx_)
    int  prevMode_ = 0;

    int  backendIdx_ = 0; // 0=Shared, 1=Exclusive (shared by both paths)
    int  deviceIdx_  = 0;
    bool devicesLoaded_ = false;
    std::vector<wa::DeviceInfo> devices_;
    char wavPath_[260] = "cap.wav";
    std::vector<std::string> logLines_;
    int  rateIdx_ = 0, bitsIdx_ = 0, chIdx_ = 1;
    bool isFloat_ = false;

    // Monitor-mode state
    bool monitorDevicesLoaded_ = false;
    std::vector<wa::DeviceInfo> capDevices_, renderDevices_;
    int  capDevIdx_ = 0, renderDevIdx_ = 0;
    int  delayMs_ = 100;
    bool monitorStarted_ = false;
    std::vector<float> capWave_, renderWave_, waveX_;  // reused each frame; no per-frame alloc
    uint32_t waveSr_ = 0;   // sample rate the buffers/x-axis were built for
    int      waveN_  = 0;   // window length in samples
};
