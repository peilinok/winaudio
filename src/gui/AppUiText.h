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

} // namespace wa::ui_text
