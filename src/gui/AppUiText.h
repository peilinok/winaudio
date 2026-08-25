#pragma once

namespace wa::ui_text {

inline constexpr const char* kApplicationLoopbackTab = "Application Loopback";
inline constexpr const char* kApplicationLoopbackRefresh = "Refresh";
inline constexpr const char* kApplicationLoopbackPidLabel = "PID";
inline constexpr const char* kApplicationLoopbackExclude = "Exclude";
inline constexpr const char* kApplicationLoopbackSessions = "Sessions";

inline constexpr const char* kAdvancedOptionsHelp =
    "Client properties are sent only when enabled for that side. When enabled, "
    "category defaults to Communications, offload defaults to false, and "
    "stream option defaults to None. Client properties require WASAPI-Shared; "
    "only Buffer applies to Exclusive. Ducking is render-only.";
inline constexpr const char* kAdvancedCaptureSection = "Capture";
inline constexpr const char* kAdvancedRenderSection = "Render";
inline constexpr const char* kAdvancedSetClientProperties = "Set client properties";
inline constexpr bool kAdvancedSetClientPropertiesDefault = false;
inline constexpr const char* kAdvancedHardwareOffload = "Hardware offload";
inline constexpr const char* kAdvancedStreamOptions[] = {
    "None",
    "Raw (bypass APO)",
    "Match format",
    "Ambisonics",
    "Post-volume loopback",
};
inline constexpr int kAdvancedStreamOptionCount =
    static_cast<int>(sizeof(kAdvancedStreamOptions) / sizeof(kAdvancedStreamOptions[0]));
inline constexpr const char* kAdvancedDuckingOptOut = "Ducking opt-out";

inline constexpr const char* kChartsPause = "Pause charts";
inline constexpr const char* kChartsResume = "Resume charts";
inline constexpr const char* kChartsPaused = "PAUSED";
inline constexpr const char* kChartsZoomOut = "Zoom out";
inline constexpr const char* kChartsZoomIn = "Zoom in";
inline constexpr const char* kChartsResetView = "Reset view";

inline constexpr const char* kLoopbackCreate = "Create Track";
inline constexpr const char* kLoopbackDestroy = "Destroy";
inline constexpr const char* kLoopbackDestroyAll = "Destroy all";
inline constexpr const char* kLoopbackTracks = "Tracks";
inline constexpr const char* kLoopbackSilentRender = "Silent render keepalive";
inline constexpr const char* kOptions = "Options";
inline constexpr const char* kCaptureOptionsPopup = "Capture options";
inline constexpr const char* kSystemDefault = "System default";
inline constexpr const char* kApply = "Apply";
inline constexpr const char* kDump = "Dump";
inline constexpr const char* kDumpStop = "Stop dump";
inline constexpr const char* kDumpCapture = "Dump capture";
inline constexpr const char* kDumpRender = "Dump render";
inline constexpr const char* kLoopbackEmptyHint = "Create a Track to capture system audio.";
inline constexpr const char* kApplicationLoopbackEmptyHint =
    "Create a Track to capture application audio.";

inline constexpr const char* kPipelineTab = "Pipeline";
inline constexpr const char* kPipelineRefresh = "Refresh";
inline constexpr const char* kPipelineSessions = "Live sessions";
inline constexpr const char* kPipelineShowSelf = "Show this process";
inline constexpr const char* kPipelineEmpty =
    "No Live sessions. Start capture or playback in another app, then Refresh.";
inline constexpr const char* kPipelineSelectHint =
    "Select a Live session to see its processing graph.";
inline constexpr const char* kPipelineGraph = "Processing graph";
inline constexpr const char* kPipelineProbe = "Probe this device";
inline constexpr const char* kPipelineEtwUnavailable = "ETW unavailable";
inline constexpr const char* kPipelineEtwListening = "ETW listening";
inline constexpr const char* kPipelineAttach = "Attach";
inline constexpr const char* kPipelineCallLog = "Call log";
inline constexpr const char* kPipelineAttached = "Attached";
inline constexpr const char* kPipelineCallLogEmpty =
    "No control-path calls yet. Attach to intercept Core Audio COM.";
inline constexpr const char* kPipelineCrossBitness = "Attach failed: cross-bitness";
inline constexpr const char* kPipelineNoDebug = "Attach failed: missing debug rights";
inline constexpr const char* kPipelineCallColIface = "Iface";
inline constexpr const char* kPipelineCallColMethod = "Method";
inline constexpr const char* kPipelineCallColArgs = "Args";
inline constexpr const char* kPipelineCallColHr = "HR";
inline constexpr const char* kPipelineCallColStream = "Stream";

} // namespace wa::ui_text
