#pragma once
#include <complex>
#include <memory>
#include <string>
#include <vector>
#include "DeviceEnumerator.h"
#include "MonitorEngine.h"
#include "Spectrogram.h"

class AppUi {
public:
    void draw();          // called each frame; polls monitor and redraws the two-column UI
    void stopAll();       // stop monitor on shutdown (idempotent)
private:
    void refreshMonitorDevices();
    void drawLeftPanel();
    void drawChartsColumn();
    void drawChartPanel(int id);
    const char* chartTitle(int id);

    wa::MonitorEngine    monitor_;
    wa::DeviceEnumerator enumerator_;
    wa::MonitorStatus    ms_;   // polled once per frame in draw(); shared by helper methods

    int  backendIdx_      = 0;
    bool playbackEnabled_ = false;
    std::vector<int>         chartOrder_      = {0, 1, 2, 3, 4, 5};
    int                      prevRenderState_ = 0;
    std::vector<std::string> logLines_;

    // Monitor device selection
    bool                        monitorDevicesLoaded_ = false;
    std::vector<wa::DeviceInfo> capDevices_;
    std::vector<wa::DeviceInfo> renderDevices_;
    int                         capDevIdx_    = 0;
    int                         renderDevIdx_ = 0;
    int                         delayMs_      = 100;
    bool                        monitorStarted_ = false;

    // Waveform buffers (50 ms window, rebuilt on rate/window change)
    std::vector<float> capWave_, renderWave_, waveX_;
    uint32_t waveSr_ = 0;
    int      waveN_  = 0;

    // Spectrum analysis (2048-point Hann FFT, dBFS)
    std::vector<std::complex<float>> workCap_, workRender_;
    std::vector<float>               specWin_;
    std::vector<float>               magCap_, magRender_, freqAxis_;
    uint64_t nextCapEnd_    = 0;
    uint64_t nextRenderEnd_ = 0;
    uint32_t specSr_        = 0;

    // Scrolling log-frequency spectrograms
    std::unique_ptr<wa::Spectrogram> capSpec_, renderSpec_;
};
