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
    void drawAdvancedModal();
    void drawChartsColumn();
    void drawChartPanel(int id);
    void drawComboPanel(bool renderSide);   // waveform + splitter + spectrogram in one cell
    void drawSpectrogramPanel(const char* plotId, wa::Spectrogram* spec, double histSec, float height, int slot);
    void drawWaveformPanel(const char* plotId, const float* wave, int n, uint32_t sr, bool haveData, float height, int slot);
    const char* chartTitle(int id);

    wa::MonitorEngine    monitor_;
    wa::DeviceEnumerator enumerator_;
    wa::MonitorStatus    ms_;   // polled once per frame in draw(); shared by helper methods

    int  backendIdx_      = 0;
    bool playbackEnabled_ = false;
    std::vector<int>         chartOrder_      = {0, 1};   // 0=cap combo, 1=ren combo
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

    wa::StreamParams capParams_{};    // Advanced 弹窗编辑;Start 时传入(运行中只读)
    wa::StreamParams renParams_{};

    // Waveform buffers — full spectrogram-history window (kSpecCols*kFftHop samples), rebuilt on rate change
    std::vector<float> capWave_, renderWave_;
    uint32_t waveSr_ = 0;
    int      waveN_  = 0;
    // Shared time X axis (seconds), linked across all time-domain charts; min/max envelope scratch.
    double xLink0_ = 0.0, xLink1_ = 0.0;
    std::vector<float> envX_, envMin_, envMax_;
    // Waveform:spectrogram height split in the combo cells (shared so cap/ren stay aligned).
    float comboRatio_ = 0.5f;
    // Last-frame plot-area hover per plot slot: locks Y that frame so in-plot wheel/drag act on X
    // only; hovering a Y ruler (not plot area) leaves Y free for per-axis zoom/pan.
    bool plotHovPrev_[4] = {};

    // Spectrum analysis (2048-point Hann FFT, dBFS)
    std::vector<std::complex<float>> workCap_, workRender_;
    std::vector<float>               specWin_;
    std::vector<float>               magCap_, magRender_;
    uint64_t nextCapEnd_    = 0;
    uint64_t nextRenderEnd_ = 0;
    uint32_t specSr_        = 0;

    // Scrolling log-frequency spectrograms
    std::unique_ptr<wa::Spectrogram> capSpec_, renderSpec_;
};
