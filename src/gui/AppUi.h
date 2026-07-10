#pragma once
#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "DeviceEnumerator.h"
#include "MonitorEngine.h"
#include "Spectrogram.h"

class AppUi {
public:
    void draw();          // called each frame; polls engines and redraws the active page
    void stopAll();       // stop engines on shutdown (idempotent)
    void pushLog(int level, const std::string& line);  // thread-safe; called from the logging pump thread
private:
    struct VisualState {
        std::vector<float> capWave, renderWave;
        uint32_t waveSr = 0;
        int      waveN  = 0;
        double   xLink0 = 0.0, xLink1 = 0.0;
        std::vector<float> envX, envMin, envMax;
        float comboRatio = 0.5f;
        bool  plotHovPrev[4] = {};

        std::vector<std::complex<float>> workCap, workRender;
        std::vector<float>               specWin;
        std::vector<float>               magCap, magRender;
        uint64_t nextCapEnd    = 0;
        uint64_t nextRenderEnd = 0;
        uint32_t specSr        = 0;

        std::unique_ptr<wa::Spectrogram> capSpec, renderSpec;
    };

    void refreshMonitorDevices();
    void drawMonitorPage();
    void drawLoopbackPage();
    void drawLoopbackLeftPanel();
    void drawLeftPanel();
    void drawAdvancedModal();
    void drawCapsModal();
    void drawFormatRegion();
    void recomputeDefaultFormat();
    void drawChartsColumn(wa::MonitorEngine& engine, const wa::MonitorStatus& status, VisualState& viz);
    void drawChartPanel(int id, wa::MonitorEngine& engine, const wa::MonitorStatus& status, VisualState& viz);
    void drawComboPanel(wa::MonitorEngine& engine, const wa::MonitorStatus& status, VisualState& viz, bool renderSide);
    void drawSpectrogramPanel(VisualState& viz, const char* plotId, wa::Spectrogram* spec, double histSec, float height, int slot);
    void drawWaveformPanel(VisualState& viz, const char* plotId, const float* wave, int n, uint32_t sr, bool haveData, float height, int slot);
    void drawLogPanel(const char* childId, bool showLevelFilter);
    const char* chartTitle(int id);
    void resetVisuals(VisualState& viz);
    void resetRenderVisuals(VisualState& viz);

    wa::MonitorEngine    monitor_;
    wa::MonitorEngine    loopback_;
    wa::DeviceEnumerator enumerator_;
    wa::MonitorStatus    ms_;   // polled once per frame in draw(); shared by helper methods
    wa::MonitorStatus    loopbackMs_;

    int  backendIdx_      = 0;
    bool playbackEnabled_ = false;
    std::vector<int>         chartOrder_      = {0, 1};   // 0=cap combo, 1=ren combo
    int                      prevRenderState_ = 0;
    std::vector<std::string> logLines_;
    std::mutex               logMutex_;    // guards pendingLog_ (pump thread → draw drains it)
    std::vector<std::string> pendingLog_;
    int                      logLevelIdx_ = 2;   // 0=Trace..4=Err; default Info

    // Monitor device selection
    bool                        monitorDevicesLoaded_ = false;
    std::vector<wa::DeviceInfo> capDevices_;
    std::vector<wa::DeviceInfo> renderDevices_;
    int                         capDevIdx_    = 0;
    int                         renderDevIdx_ = 0;
    int                         loopbackDevIdx_ = 0;
    int                         delayMs_      = 100;
    bool                        monitorStarted_ = false;
    bool                        loopbackStarted_ = false;
    bool                        loopbackSilentRender_ = true;

    wa::StreamParams capParams_{};    // Advanced 弹窗编辑;Start 时传入(运行中只读)
    wa::StreamParams renParams_{};

    // Format selection state
    wa::AudioFormat        selectedFmt_{};
    bool                   haveFmt_         = false;
    int                    fmtChoiceIdx_    = 0;
    char                   fmtCustom_[32]   = "48000/16/2";
    int                    fmtBackendShown_ = -1;
    int                    capDevShown_     = -1;
    wa::DeviceCapabilities capsCache_{};

    VisualState monitorViz_;
    VisualState loopbackViz_;
};
