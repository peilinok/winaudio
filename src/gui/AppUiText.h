#pragma once
#include "GuiLanguage.h"
#include "PhraseCatalog.h"
#include "PipelineGraph.h"
#include <cstdio>
#include <string>

namespace wa::ui_text {

// One row: identifier, English literal, Chinese slot (nullptr or "" = fallback).
#define WA_UI_TEXT_ROWS \
    WA_UI_TEXT(kApplicationLoopbackTab, "Application Loopback", "Application Loopback") \
    WA_UI_TEXT(kApplicationLoopbackRefresh, "Refresh", "刷新") \
    WA_UI_TEXT(kApplicationLoopbackPidLabel, "PID", "PID") \
    WA_UI_TEXT(kApplicationLoopbackExclude, "Exclude", "排除") \
    WA_UI_TEXT(kApplicationLoopbackSessions, "Sessions", "会话") \
    WA_UI_TEXT(kFormatReference, "Format reference", "格式参考") \
    WA_UI_TEXT(kFormatReferenceNone, "None", "无") \
    WA_UI_TEXT(kFormatReferenceDefaultRender, "Default render device", "默认 Render 设备") \
    WA_UI_TEXT(kAutomatic, "Automatic", "自动") \
    WA_UI_TEXT(kCapabilities, "Capabilities...", "能力...") \
    WA_UI_TEXT(kAdvancedOptionsHelp, \
        "Client properties are sent only when enabled for that side. When enabled, " \
        "category defaults to Communications, offload defaults to false, and " \
        "stream option defaults to None. Client properties require WASAPI-Shared; " \
        "only Buffer applies to Exclusive. Ducking is render-only.", \
        "仅在该侧启用时发送客户端属性。启用后，类别默认为 Communications，offload 默认为 false，" \
        "流选项默认为无。客户端属性需要 WASAPI Shared；只有 Buffer 适用于 Exclusive。Ducking 仅用于 Render。") \
    WA_UI_TEXT(kAdvancedCaptureSection, "Capture", "Capture") \
    WA_UI_TEXT(kAdvancedSetClientProperties, "Set client properties", "设置客户端属性") \
    WA_UI_TEXT(kAdvancedHardwareOffload, "Hardware offload", "硬件 offload") \
    WA_UI_TEXT(kStreamOptionRaw, "Raw (bypass APO)", "Raw（绕过 APO）") \
    WA_UI_TEXT(kStreamOptionMatch, "Match format", "匹配格式") \
    WA_UI_TEXT(kStreamOptionAmbisonics, "Ambisonics", "Ambisonics") \
    WA_UI_TEXT(kStreamOptionPostVolume, "Post-volume loopback", "音量后 Loopback") \
    WA_UI_TEXT(kAdvancedDuckingOptOut, "Ducking opt-out", "Ducking 退出") \
    WA_UI_TEXT(kChartsPause, "Pause charts", "暂停图表") \
    WA_UI_TEXT(kChartsResume, "Resume charts", "继续图表") \
    WA_UI_TEXT(kChartsPaused, "PAUSED", "已暂停") \
    WA_UI_TEXT(kChartsZoomOut, "Zoom out", "缩小") \
    WA_UI_TEXT(kChartsZoomIn, "Zoom in", "放大") \
    WA_UI_TEXT(kChartsResetView, "Reset view", "重置视图") \
    WA_UI_TEXT(kLoopbackCreate, "Create Track", "创建 Track") \
    WA_UI_TEXT(kLoopbackDestroy, "Destroy", "销毁") \
    WA_UI_TEXT(kLoopbackDestroyAll, "Destroy all", "全部销毁") \
    WA_UI_TEXT(kLoopbackTracks, "Tracks", "Tracks") \
    WA_UI_TEXT(kLoopbackSilentRender, "Silent render keepalive", "静音 Render 保活") \
    WA_UI_TEXT(kOptions, "Options", "选项") \
    WA_UI_TEXT(kMmsysCpl, "mmsys.cpl", "mmsys.cpl") \
    WA_UI_TEXT(kMsSettings, "ms-settings", "ms-settings") \
    WA_UI_TEXT(kMmsysCplFailed, "mmsys.cpl: ShellExecute failed", "mmsys.cpl：ShellExecute 失败") \
    WA_UI_TEXT(kMsSettingsFailed, "ms-settings: ShellExecute failed", "ms-settings：ShellExecute 失败") \
    WA_UI_TEXT(kCaptureOptionsPopup, "Capture options", "Capture 选项") \
    WA_UI_TEXT(kSystemDefault, "System default", "系统默认") \
    WA_UI_TEXT(kApply, "Apply", "应用") \
    WA_UI_TEXT(kDump, "Dump", "转储") \
    WA_UI_TEXT(kDumpStop, "Stop dump", "停止转储") \
    WA_UI_TEXT(kDumpCapture, "Dump capture", "转储 Capture") \
    WA_UI_TEXT(kDumpRender, "Dump render", "转储 Render") \
    WA_UI_TEXT(kLoopbackEmptyHint, "Create a Track to capture system audio.", "创建 Track 以采集系统音频。") \
    WA_UI_TEXT(kRenderTab, "Render", "Render") \
    WA_UI_TEXT(kRenderEmptyHint, "Create a Track to play to a render endpoint.", "创建 Track 以播放到 Render 端点。") \
    WA_UI_TEXT(kRenderEndpoint, "Render endpoint", "Render 端点") \
    WA_UI_TEXT(kRenderLayout, "Layout", "布局") \
    WA_UI_TEXT(kRenderCustomIgnored, "Sample rate and bit depth are ignored.", "采样率和位深将被忽略。") \
    WA_UI_TEXT(kRenderChannelsPaused, "All channels paused", "所有声道已暂停") \
    WA_UI_TEXT(kRenderPause, "Pause", "暂停") \
    WA_UI_TEXT(kRenderContinue, "Continue", "继续") \
    WA_UI_TEXT(kRenderPlayOnce, "Play once", "播放一次") \
    WA_UI_TEXT(kRenderNoPhrase, "No phrase", "无语句") \
    WA_UI_TEXT(kRenderVolume, "Volume", "音量") \
    WA_UI_TEXT(kApplicationLoopbackEmptyHint, "Create a Track to capture application audio.", "创建 Track 以采集应用音频。") \
    WA_UI_TEXT(kLogEmpty, "(empty)", "（空）") \
    WA_UI_TEXT(kLogExpand, "+", nullptr) \
    WA_UI_TEXT(kLogCollapse, "-", "") \
    WA_UI_TEXT(kPipelineTab, "Pipeline", "Pipeline") \
    WA_UI_TEXT(kPipelineSessions, "Live sessions", "Live 会话") \
    WA_UI_TEXT(kPipelineShowSelf, "Show this process", "显示本进程") \
    WA_UI_TEXT(kPipelineEmpty, \
        "No Live sessions. Start capture or playback in another app, then Refresh.", \
        "没有 Live 会话。请在其他应用中开始采集或播放，然后刷新。") \
    WA_UI_TEXT(kPipelineSelectHint, \
        "Select a Live session to see its processing graph.", \
        "选择一个 Live 会话以查看其处理图。") \
    WA_UI_TEXT(kPipelineGraph, "Processing graph", "处理图") \
    WA_UI_TEXT(kPipelineProbe, "Probe this device", "探测此设备") \
    WA_UI_TEXT(kPipelineEtwUnavailable, "ETW unavailable", "ETW 不可用") \
    WA_UI_TEXT(kPipelineEtwListening, "ETW listening", "ETW 监听中") \
    WA_UI_TEXT(kPipelineAttach, "Attach", "附加") \
    WA_UI_TEXT(kPipelineCallLog, "Call log", "调用日志") \
    WA_UI_TEXT(kPipelineAttached, "Attached", "已附加") \
    WA_UI_TEXT(kPipelineCallLogEmpty, \
        "No control-path calls yet. Attach to intercept Core Audio COM.", \
        "尚无控制路径调用。附加以拦截 Core Audio COM。") \
    WA_UI_TEXT(kPipelineCallLogWaiting, \
        "Attached. Call log waits for the next control-path call (Initialize, Start, Stop, " \
        "GetService). Enable Record pump metadata to see GetBuffer/ReleaseBuffer.", \
        "已附加。调用日志等待下一次控制路径调用（Initialize、Start、Stop、GetService）。" \
        "启用“记录 pump 元数据”可查看 GetBuffer/ReleaseBuffer。") \
    WA_UI_TEXT(kPipelineCallLogWaitingPump, \
        "Attached with pump metadata on. Waiting for GetBuffer/ReleaseBuffer. If this stays " \
        "empty, the process may not be using Core Audio COM on this device.", \
        "已附加并开启 pump 元数据。正在等待 GetBuffer/ReleaseBuffer。若一直为空，" \
        "该进程可能未在此设备上使用 Core Audio COM。") \
    WA_UI_TEXT(kPipelineFlowCapture, "capture", "Capture") \
    WA_UI_TEXT(kPipelineFlowRender, "render", "Render") \
    WA_UI_TEXT(kPipelineMuteYes, "yes", "是") \
    WA_UI_TEXT(kPipelineMuteNo, "no", "否") \
    WA_UI_TEXT(kPipelineCrossBitness, "Attach failed: cross-bitness", "附加失败：位数不匹配") \
    WA_UI_TEXT(kPipelineNoDebug, "Attach failed: missing debug rights", "附加失败：缺少调试权限") \
    WA_UI_TEXT(kPipelineCallColIface, "Iface", "接口") \
    WA_UI_TEXT(kPipelineCallColMethod, "Method", "方法") \
    WA_UI_TEXT(kPipelineCallColArgs, "Args", "参数") \
    WA_UI_TEXT(kPipelineCallColHr, "HR", "HR") \
    WA_UI_TEXT(kPipelineCallColStream, "Stream", "流") \
    WA_UI_TEXT(kPipelinePump, "Record pump metadata", "记录 pump 元数据") \
    WA_UI_TEXT(kPipelineXruns, "xruns", "xruns") \
    WA_UI_TEXT(kRefreshDevices, "Refresh devices", "刷新设备") \
    WA_UI_TEXT(kClose, "Close", "关闭") \
    WA_UI_TEXT(kFormat, "Format", "格式") \
    WA_UI_TEXT(kIdle, "Idle", "空闲") \
    WA_UI_TEXT(kRunning, "Running", "运行中") \
    WA_UI_TEXT(kError, "Error", "错误") \
    WA_UI_TEXT(kControl, "Control", "控制") \
    WA_UI_TEXT(kSource, "Source", "来源") \
    WA_UI_TEXT(kDevices, "Devices", "设备") \
    WA_UI_TEXT(kStatus, "Status", "状态") \
    WA_UI_TEXT(kStart, "Start", "开始") \
    WA_UI_TEXT(kStop, "Stop", "停止") \
    WA_UI_TEXT(kBackend, "Backend", "后端") \
    WA_UI_TEXT(kDelay, "Delay", "延迟") \
    WA_UI_TEXT(kCategory, "Category", "类别") \
    WA_UI_TEXT(kResetToSystemDefaults, "Reset to system defaults", "重置为系统默认") \
    WA_UI_TEXT(kDeviceCapabilities, "Device capabilities", "设备能力") \
    WA_UI_TEXT(kCaptureCapabilities, "Capture capabilities", "Capture 能力")

#define WA_UI_TEXT(name, en, zh) inline constexpr const char* name = en;
WA_UI_TEXT_ROWS
#undef WA_UI_TEXT

// Same English literal as another row, so they share that row's Chinese slot.
inline constexpr const char* kPipelineRefresh = kApplicationLoopbackRefresh;
inline constexpr const char* kAdvancedRenderSection = kRenderTab;

inline constexpr bool kAdvancedSetClientPropertiesDefault = false;
inline constexpr const char* kAdvancedStreamOptions[] = {
    kFormatReferenceNone,
    kStreamOptionRaw,
    kStreamOptionMatch,
    kStreamOptionAmbisonics,
    kStreamOptionPostVolume,
};
inline constexpr int kAdvancedStreamOptionCount =
    static_cast<int>(sizeof(kAdvancedStreamOptions) / sizeof(kAdvancedStreamOptions[0]));

inline wa::gui_language::Language& currentLanguageStorage() {
    static wa::gui_language::Language language = wa::gui_language::Language::En;
    return language;
}

inline wa::gui_language::Language language() { return currentLanguageStorage(); }

inline void setLanguage(wa::gui_language::Language language) {
    currentLanguageStorage() = language;
}

struct CatalogEntry {
    const char* en;
    const char* zh;
};

inline const char* get(const char* english) {
    static constexpr const char* kEmpty = "";
    if (!english) return kEmpty;
    if (language() != wa::gui_language::Language::ZhCn) return english;
    static const CatalogEntry kRows[] = {
#define WA_UI_TEXT(name, en, zh) {name, zh},
        WA_UI_TEXT_ROWS
#undef WA_UI_TEXT
    };
    for (const CatalogEntry& row : kRows) {
        if (row.en != english) continue;
        if (row.zh != nullptr && row.zh[0] != '\0') return row.zh;
        return english;
    }
    return english;
}

// Visible wording plus a language-stable ImGui id. The pointer is valid until
// enough later id() calls reuse the slot. ImGui copies the text during the call.
inline const char* id(const char* english) {
    if (!english) return get(english);
    struct Slot {
        char buf[2048];
    };
    static thread_local Slot slots[8];
    static thread_local unsigned next = 0;
    char* buf = slots[next++ % 8].buf;
    const char* visible = get(english);
    const int wrote = std::snprintf(buf, sizeof(slots[0].buf), "%s###%s", visible, english);
    if (wrote < 0 || wrote >= static_cast<int>(sizeof(slots[0].buf))) return english;
    return buf;
}

inline const char* channelPhraseText(ChannelPhrase phrase) {
    switch (phrase) {
    case ChannelPhrase::FrontLeft: return "Front left";
    case ChannelPhrase::FrontRight: return "Front right";
    case ChannelPhrase::FrontCenter: return "Front center";
    case ChannelPhrase::LowFrequency: return "Low frequency";
    case ChannelPhrase::BackLeft: return "Back left";
    case ChannelPhrase::BackRight: return "Back right";
    case ChannelPhrase::BackCenter: return "Back center";
    case ChannelPhrase::SideLeft: return "Side left";
    case ChannelPhrase::SideRight: return "Side right";
    case ChannelPhrase::FrontLeftOfCenter: return "Front left of center";
    case ChannelPhrase::FrontRightOfCenter: return "Front right of center";
    case ChannelPhrase::TopCenter: return "Top center";
    case ChannelPhrase::TopFrontLeft: return "Top front left";
    case ChannelPhrase::TopFrontCenter: return "Top front center";
    case ChannelPhrase::TopFrontRight: return "Top front right";
    case ChannelPhrase::TopBackLeft: return "Top back left";
    case ChannelPhrase::TopBackCenter: return "Top back center";
    case ChannelPhrase::TopBackRight: return "Top back right";
    case ChannelPhrase::Channel1: return "channel 1";
    case ChannelPhrase::Channel2: return "channel 2";
    case ChannelPhrase::Channel3: return "channel 3";
    case ChannelPhrase::Channel4: return "channel 4";
    case ChannelPhrase::Channel5: return "channel 5";
    case ChannelPhrase::Channel6: return "channel 6";
    case ChannelPhrase::Channel7: return "channel 7";
    case ChannelPhrase::Channel8: return "channel 8";
    case ChannelPhrase::None:
    case ChannelPhrase::Count:
        break;
    }
    return get(kRenderNoPhrase);
}

inline std::string formatLiveSessionTooltip(const LiveSessionView& row) {
    char vol[16];
    std::snprintf(vol, sizeof(vol), "%.2f", static_cast<double>(row.sessionVolume));
    std::string out;
    out.reserve(128 + row.processName.size() + row.deviceName.size() + row.state.size());
    out += get((row.flow == PipelineFlow::Capture) ? kPipelineFlowCapture : kPipelineFlowRender);
    out += "\nprocess: ";
    out += row.processName;
    out += "\npid: ";
    out += std::to_string(row.processId);
    out += "\ndevice: ";
    out += row.deviceName;
    out += "\nvolume: ";
    out += vol;
    out += "\nmute: ";
    out += get(row.sessionMute ? kPipelineMuteYes : kPipelineMuteNo);
    out += "\nstate: ";
    out += row.state;
    return out;
}

inline const char* pipelineCallLogEmptyText(bool attached, bool pumpEnabled) {
    if (!attached) return get(kPipelineCallLogEmpty);
    return get(pumpEnabled ? kPipelineCallLogWaitingPump : kPipelineCallLogWaiting);
}

} // namespace wa::ui_text
