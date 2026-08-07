#pragma once
#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "AudioSessionEnumerator.h"
#include "DeviceEnumerator.h"
#include "MonitorEngine.h"
#include "Spectrogram.h"

class AppUi {
public:
    void draw();          // called each frame; polls engines and redraws the active page
    void stopAll();       // stop engines on shutdown (idempotent)
    void pushLog(int level, const std::string& line);  // thread-safe; called from the logging pump thread
private:
    struct AppLoopbackStartJob {
        std::mutex mtx;
        bool done = false;
        wa::Result result = wa::Result::Ok();
        std::unique_ptr<wa::MonitorEngine> engine;
    };

    struct VisualState {
        std::vector<float> capWave, renderWave;
        std::vector<std::vector<float>> capChannelWaves;
        uint32_t waveSr = 0;
        int      waveN  = 0;
        uint32_t capWaveChannels = 0;
        double   xLink0 = 0.0, xLink1 = 0.0;
        std::vector<float> envX, envMin, envMax;
        float comboRatio = 0.5f;
        bool  plotHovPrev[18] = {}; // capture wave/spec slots for up to 8 channels + render slots

        std::vector<std::complex<float>> workCap, workRender;
        std::vector<float>               specWin;
        std::vector<std::vector<float>>  capSpecWindows;
        std::vector<float>               magCap, magRender;
        uint64_t nextCapEnd    = 0;
        uint64_t nextRenderEnd = 0;
        uint32_t specSr        = 0;
        uint32_t capSpecChannels = 0;

        std::unique_ptr<wa::Spectrogram> capSpec, renderSpec;
        std::vector<std::unique_ptr<wa::Spectrogram>> capChannelSpecs;
    };

    void refreshMonitorDevices();
    void refreshApplicationLoopbackSessions();
    void beginApplicationLoopbackStart(uint32_t pid, wa::ProcessLoopbackMode mode);
    void drainApplicationLoopbackStart();
    void drawMonitorPage();
    void drawLoopbackPage();
    void drawApplicationLoopbackPage();
    void drawLoopbackLeftPanel();
    void drawApplicationLoopbackLeftPanel();
    void drawLeftPanel();
    void drawAdvancedModal();
    void drawCapsModal();
    void drawFormatRegion();
    void recomputeDefaultFormat();
    void ensureRunningVisuals(const wa::MonitorStatus& status, VisualState& viz,
                              bool includeRender);
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
    std::unique_ptr<wa::MonitorEngine> appLoopback_ = std::make_unique<wa::MonitorEngine>();
    wa::DeviceEnumerator enumerator_;
    wa::AudioSessionEnumerator sessionEnumerator_;
    wa::MonitorStatus    ms_;   // polled once per frame in draw(); shared by helper methods
    wa::MonitorStatus    loopbackMs_;
    wa::MonitorStatus    appLoopbackMs_;

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
    bool                        appLoopbackStarted_ = false;
    bool                        appLoopbackStartPending_ = false;
    bool                        appLoopbackExclude_ = false; // checkbox; false = IncludeTree
    bool                        appLoopbackSessionsLoaded_ = false;
    std::vector<wa::AudioSessionProcess> appLoopbackSessions_;
    int                         appLoopbackSessionIdx_ = -1;
    char                        appLoopbackPid_[32] = "";
    wa::ProcessLoopbackMode     appLoopbackMode_ = wa::ProcessLoopbackMode::IncludeTree;
    std::thread                 appLoopbackStartThread_;
    std::shared_ptr<AppLoopbackStartJob> appLoopbackStartJob_;

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
    VisualState appLoopbackViz_;
};
