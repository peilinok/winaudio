#pragma once
#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "AudioSessionEnumerator.h"
#include "CaptureTrackList.h"
#include "ChartHost.h"
#include "CreateRecipe.h"
#include "DeviceEnumerator.h"
#include "DumpUi.h"
#include "EndpointGraphReader.h"
#include "EtwInitialize.h"
#include "LiveSessionEnumerator.h"
#include "MonitorEngine.h"
#include "PipelineGraph.h"
#include "SharedProbe.h"
#include "Spectrogram.h"
#include "TrackScopeReader.h"

class AppUi {
public:
    void draw();          // called each frame; polls engines and redraws the active page
    void stopAll();       // stop engines on shutdown (idempotent)
    void pushLog(int level, const std::string& line);  // thread-safe; called from the logging pump thread
private:
    struct VisualState {
        std::vector<float> capWave, renderWave;
        std::vector<std::vector<float>> capChannelWaves;
        uint32_t waveSr = 0;
        int      waveN  = 0;
        uint32_t capWaveChannels = 0;
        double   xLink0 = 0.0, xLink1 = 0.0;
        std::vector<float> envX, envMin, envMax;
        float comboRatio = 0.5f;
        bool  chartsFrozen = false; // pause chart data refresh only; audio continues
        bool  resetYAxes = false;   // one-shot: next draw restores default Y on all plots
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
    void refreshPipelineSessions();
    void rebuildPipelineGraph();
    void applyPipelineJoin();
    void runPipelineProbe();
    void drawMonitorPage();
    void drawPipelinePage();
    void drawLoopbackPage();
    void drawApplicationLoopbackPage();
    void drawLoopbackLeftPanel();
    void drawApplicationLoopbackLeftPanel();
    enum class DumpPickKind { None, Loopback, AppLoopback, MonitorCap, MonitorRen };
    void drawDumpControls(wa::CaptureTrackList& list, const wa::CaptureTrackStatus& t,
                          DumpPickKind kind, const char* destroyedLog);
    void beginDumpPick(DumpPickKind kind, wa::TrackId trackId = 0);
    void applyDumpPick();
    void drawStackedCaptureTrackHosts(wa::CaptureTrackList& list,
                                      std::vector<std::pair<wa::TrackId, VisualState>>& viz,
                                      const char* emptyHint);
    void drawLeftPanel();
    void drawAdvancedModal();
    void drawCapsModal();
    void drawFormatRegion();
    void recomputeDefaultFormat();
    void recomputeLoopbackFormat();
    void drawFormatRecipe(wa::create_recipe::FormatState& st,
                          const std::vector<wa::AudioFormat>& candidates,
                          const wa::AudioFormat& defaultDisplay,
                          const char* comboId, wa::StreamParams& params);
    void drawCaptureOptions(wa::StreamParams& params);
    void ensureRunningVisuals(const wa::MonitorStatus& status, VisualState& viz,
                              bool includeRender);
    // Chart Host: right-hand charts column (ensure + toolbar + panels + clear resetYAxes).
    // engine may be null (capture-only toolbar only). order non-null only for DualReorderable.
    void drawChartHost(wa::MonitorEngine* engine, const wa::MonitorStatus& status, VisualState& viz,
                       wa::chart_host::Mode mode, const char* caption = nullptr,
                       std::vector<int>* order = nullptr, wa::ScopeReader* captureReader = nullptr);
    void drawChartsFreezeToolbar(VisualState& viz, const wa::MonitorStatus& status);
    void drawChartPanel(int id, wa::ScopeReader& captureReader, wa::ScopeReader* renderReader,
                        const wa::MonitorStatus& status, VisualState& viz);
    void drawComboPanel(wa::ScopeReader& reader, const wa::MonitorStatus& status, VisualState& viz,
                        bool renderSide);
    void drawSpectrogramPanel(VisualState& viz, const char* plotId, wa::Spectrogram* spec, double histSec, float height, int slot);
    void drawWaveformPanel(VisualState& viz, const char* plotId, const float* wave, int n, uint32_t sr, bool haveData, float height, int slot);
    void drawLogPanel(const char* childId, bool showLevelFilter);
    const char* chartTitle(int id);
    void resetVisuals(VisualState& viz);
    void resetRenderVisuals(VisualState& viz);

    wa::MonitorEngine    monitor_;
    wa::CaptureTrackList loopbackTracks_;
    wa::CaptureTrackList appLoopbackTracks_;
    wa::DeviceEnumerator enumerator_;
    wa::AudioSessionEnumerator sessionEnumerator_;
    wa::LiveSessionEnumerator liveSessionEnumerator_;
    wa::EndpointGraphReader endpointGraphReader_;
    wa::MonitorStatus    ms_;   // polled once per frame in draw(); shared by helper methods

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
    bool                        loopbackSilentRender_ = true;
    wa::create_recipe::CreateRecipe loopbackRecipe_{};
    bool                        appLoopbackExclude_ = false; // create recipe; false = IncludeTree
    bool                        appLoopbackSessionsLoaded_ = false;
    std::vector<wa::AudioSessionProcess> appLoopbackSessions_;
    int                         appLoopbackSessionIdx_ = -1;
    char                        appLoopbackPid_[32] = "";
    wa::create_recipe::CreateRecipe appLoopbackRecipe_{};

    bool pipelineSessionsLoaded_ = false;
    bool pipelineShowSelf_ = false;
    int  pipelineSelected_ = -1;
    std::vector<wa::LiveSessionView> pipelineSessions_;
    std::vector<wa::PipelineNode> pipelineNodes_;
    std::vector<wa::ProbeSlice> pipelineProbes_;
    bool pipelineProbing_ = false;
    wa::EtwInitializeWatch pipelineEtw_;
    bool pipelineEtwStarted_ = false;
    wa::EndpointSnapshot pipelineEndpoint_;
    wa::EtwInitializeHint pipelineEtwHint_{};

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

    wa::dump_ui::FolderPicker dumpPicker_;
    DumpPickKind              dumpPickKind_ = DumpPickKind::None;
    wa::TrackId               dumpPickTrackId_ = 0;

    VisualState monitorViz_;
    std::vector<std::pair<wa::TrackId, VisualState>> loopbackViz_;
    std::vector<std::pair<wa::TrackId, VisualState>> appLoopbackViz_;
};
