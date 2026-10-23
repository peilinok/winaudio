#pragma once
#include "AudioFormat.h"
#include "Capabilities.h"
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
    WA_UI_TEXT(kRenderFormatUnits, "%u Hz, %u-bit", "%u Hz，%u 位") \
    WA_UI_TEXT(kLayoutMono, "Mono", "单声道") \
    WA_UI_TEXT(kLayoutStereo, "Stereo", "立体声") \
    WA_UI_TEXT(kLayoutQuad, "Quad", "四声道") \
    WA_UI_TEXT(kLayout51, "5.1", "5.1") \
    WA_UI_TEXT(kLayout71, "7.1", "7.1") \
    WA_UI_TEXT(kLayoutMask, "mask 0x%X", "掩码 0x%X") \
    WA_UI_TEXT(kLayoutChannelCount, "%u channels", "%u 声道") \
    WA_UI_TEXT(kRenderChannelsPaused, "All channels paused", "所有声道已暂停") \
    WA_UI_TEXT(kRenderPause, "Pause", "暂停") \
    WA_UI_TEXT(kRenderContinue, "Continue", "继续") \
    WA_UI_TEXT(kRenderPlayOnce, "Play once", "播放一次") \
    WA_UI_TEXT(kRenderNoPhrase, "No phrase", "无语句") \
    WA_UI_TEXT(kRenderVolume, "Volume", "音量") \
    WA_UI_TEXT(kPhraseFrontLeft, "Front left", "前左") \
    WA_UI_TEXT(kPhraseFrontRight, "Front right", "前右") \
    WA_UI_TEXT(kPhraseFrontCenter, "Front center", "前中") \
    WA_UI_TEXT(kPhraseLowFrequency, "Low frequency", "低频") \
    WA_UI_TEXT(kPhraseBackLeft, "Back left", "后左") \
    WA_UI_TEXT(kPhraseBackRight, "Back right", "后右") \
    WA_UI_TEXT(kPhraseBackCenter, "Back center", "后中") \
    WA_UI_TEXT(kPhraseSideLeft, "Side left", "侧左") \
    WA_UI_TEXT(kPhraseSideRight, "Side right", "侧右") \
    WA_UI_TEXT(kPhraseFrontLeftOfCenter, "Front left of center", "前左中") \
    WA_UI_TEXT(kPhraseFrontRightOfCenter, "Front right of center", "前右中") \
    WA_UI_TEXT(kPhraseTopCenter, "Top center", "顶中") \
    WA_UI_TEXT(kPhraseTopFrontLeft, "Top front left", "顶前左") \
    WA_UI_TEXT(kPhraseTopFrontCenter, "Top front center", "顶前中") \
    WA_UI_TEXT(kPhraseTopFrontRight, "Top front right", "顶前右") \
    WA_UI_TEXT(kPhraseTopBackLeft, "Top back left", "顶后左") \
    WA_UI_TEXT(kPhraseTopBackCenter, "Top back center", "顶后中") \
    WA_UI_TEXT(kPhraseTopBackRight, "Top back right", "顶后右") \
    WA_UI_TEXT(kPhraseChannel1, "channel 1", "声道 1") \
    WA_UI_TEXT(kPhraseChannel2, "channel 2", "声道 2") \
    WA_UI_TEXT(kPhraseChannel3, "channel 3", "声道 3") \
    WA_UI_TEXT(kPhraseChannel4, "channel 4", "声道 4") \
    WA_UI_TEXT(kPhraseChannel5, "channel 5", "声道 5") \
    WA_UI_TEXT(kPhraseChannel6, "channel 6", "声道 6") \
    WA_UI_TEXT(kPhraseChannel7, "channel 7", "声道 7") \
    WA_UI_TEXT(kPhraseChannel8, "channel 8", "声道 8") \
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
    WA_UI_TEXT(kPipelineDetach, "Detach", "分离") \
    WA_UI_TEXT(kPipelineRecord, "Record calls", "录制调用") \
    WA_UI_TEXT(kPipelineSaveCalls, "Save calls", "保存调用") \
    WA_UI_TEXT(kPipelineColAttach, "Hook", "Hook") \
    WA_UI_TEXT(kPipelineAttachDetached, "Detached", "未附加") \
    WA_UI_TEXT(kPipelineAttachFailed, "Failed", "失败") \
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
    WA_UI_TEXT(kPipelineTipProcess, "process: ", "进程：") \
    WA_UI_TEXT(kPipelineTipPid, "pid: ", "PID：") \
    WA_UI_TEXT(kPipelineTipDevice, "device: ", "设备：") \
    WA_UI_TEXT(kPipelineTipVolume, "volume: ", "音量：") \
    WA_UI_TEXT(kPipelineTipMute, "mute: ", "静音：") \
    WA_UI_TEXT(kPipelineTipState, "state: ", "状态：") \
    WA_UI_TEXT(kPipelineColFlow, "Flow", "流向") \
    WA_UI_TEXT(kPipelineColProcess, "Process", "进程") \
    WA_UI_TEXT(kPipelineColDevice, "Device", "设备") \
    WA_UI_TEXT(kPipelineColVol, "Vol", "音量") \
    WA_UI_TEXT(kPipelineColMute, "Mute", "静音") \
    WA_UI_TEXT(kPipelineColState, "State", "状态") \
    WA_UI_TEXT(kPipelineCrossBitness, "Attach failed: cross-bitness", "附加失败：位数不匹配") \
    WA_UI_TEXT(kPipelineNoDebug, "Attach failed: missing debug rights", "附加失败：缺少调试权限") \
    WA_UI_TEXT(kPipelineCallColIface, "Iface", "接口") \
    WA_UI_TEXT(kPipelineCallColMethod, "Method", "方法") \
    WA_UI_TEXT(kPipelineCallColCall, "Call", "调用") \
    WA_UI_TEXT(kPipelineCallColArgs, "Args", "参数") \
    WA_UI_TEXT(kPipelineCallColHr, "HR", "HR") \
    WA_UI_TEXT(kPipelineCallColStream, "Stream", "流") \
    WA_UI_TEXT(kPipelineCallColType, "Type", "类型") \
    WA_UI_TEXT(kPipelineCallColName, "Name", "名称") \
    WA_UI_TEXT(kPipelineCallColPre, "Pre", "调用前") \
    WA_UI_TEXT(kPipelineCallColPost, "Post", "调用后") \
    WA_UI_TEXT(kPipelineCallParams, "Parameters", "参数") \
    WA_UI_TEXT(kPipelineCallParamsSelectHint, "Select a call log row to view parameters.", "选择一条调用记录以查看参数。") \
    WA_UI_TEXT(kPipelineCallParamsEmpty, "No parameters for this call.", "此调用无参数。") \
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
    WA_UI_TEXT(kCaptureCapabilities, "Capture capabilities", "Capture 能力") \
    WA_UI_TEXT(kMonitorTab, "Monitor", nullptr) \
    WA_UI_TEXT(kLoopbackTab, "Loopback", "Loopback") \
    WA_UI_TEXT(kCaptureDevice, "Capture device", "Capture 设备") \
    WA_UI_TEXT(kRenderDevice, "Render device", "Render 设备") \
    WA_UI_TEXT(kNoDevices, "(no devices)", "（无设备）") \
    WA_UI_TEXT(kNoRenderDevices, "(no render devices)", "（无 Render 设备）") \
    WA_UI_TEXT(kEnhancementsOnShort, "FX on", "增强开") \
    WA_UI_TEXT(kEnhancementsOffShort, "FX off", "增强关") \
    WA_UI_TEXT(kEnhancementsUnknownShort, "FX ?", "增强未知") \
    WA_UI_TEXT(kEnhancementsLabel, "Enhancements", "音频增强") \
    WA_UI_TEXT(kEnhancementsEnable, "Enable FX", "开启增强") \
    WA_UI_TEXT(kEnhancementsDisable, "Disable FX", "关闭增强") \
    WA_UI_TEXT(kEnhancementsLockedStreaming, \
        "Stop all streams before changing endpoint enhancements.", \
        "请先停止全部流，再改音频增强。") \
    WA_UI_TEXT(kEnhancementsSetFailed, "Enhancements set failed", "设置音频增强失败") \
    WA_UI_TEXT(kEnhancementOptions, "Enhancement options", "增强选项") \
    WA_UI_TEXT(kStreamEffects, "Stream effects", "流效果") \
    WA_UI_TEXT(kProbeStreamEffects, "Probe stream effects", "探测流效果") \
    WA_UI_TEXT(kStreamEffectsNone, "(none)", "（无）") \
    WA_UI_TEXT(kStreamEffectsHint,         "Click Probe to list AEC / Noise Suppression (Win11).",         "点击「探测流效果」可列出 AEC / 噪声抑制（需 Win11）。") \
    WA_UI_TEXT(kStreamEffectsNeedWin11,         "Requires Windows 11 (build 22000+); opens a short Shared stream (no Start).",         "需要 Windows 11（build 22000+）；会短暂打开 Shared 流（不 Start）。") \
    WA_UI_TEXT(kStreamEffectOn, "On", "开") \
    WA_UI_TEXT(kStreamEffectOff, "Off", "关") \
    WA_UI_TEXT(kSystemAudio, "System audio", "系统音频") \
    WA_UI_TEXT(kApplicationLoopbackNoSessions, "(none)", "（无）") \
    WA_UI_TEXT(kDelayMs, "Delay (ms)", "延迟 (ms)") \
    WA_UI_TEXT(kWasapiShared, "WASAPI-Shared", nullptr) \
    WA_UI_TEXT(kWasapiExclusive, "WASAPI-Exclusive", nullptr) \
    WA_UI_TEXT(kSyncPlayback, "Sync playback", "同步播放") \
    WA_UI_TEXT(kFormatDefault, " (default)", "（默认）") \
    WA_UI_TEXT(kAudioParametersAdvanced, "Audio parameters (advanced)", "音频参数（高级）") \
    WA_UI_TEXT(kRunningParamsReadOnly, \
        "Running: parameters are read-only. Stop the monitor to change them.", \
        "运行中：参数只读。请先停止 Monitor 再修改。") \
    WA_UI_TEXT(kStreamOptionCaption, "Stream option", "流选项") \
    WA_UI_TEXT(kBufferMs, "Buffer (ms)", "缓冲 (ms)") \
    WA_UI_TEXT(kCategoryOther, "Other", "其他") \
    WA_UI_TEXT(kCategoryCommunications, "Communications", nullptr) \
    WA_UI_TEXT(kCategoryMedia, "Media", nullptr) \
    WA_UI_TEXT(kCategoryMovie, "Movie", "电影") \
    WA_UI_TEXT(kCategoryGameChat, "Game chat", "游戏聊天") \
    WA_UI_TEXT(kCategorySpeech, "Speech", "语音") \
    WA_UI_TEXT(kCategorySoundEffects, "Sound effects", "音效") \
    WA_UI_TEXT(kCategoryGameMedia, "Game media", "游戏媒体") \
    WA_UI_TEXT(kOriginMix, "Mix", nullptr) \
    WA_UI_TEXT(kOriginDevice, "Device", "设备") \
    WA_UI_TEXT(kOriginOem, "OEM", nullptr) \
    WA_UI_TEXT(kOriginStandard, "Standard", "标准") \
    WA_UI_TEXT(kSources, "Sources", "来源") \
    WA_UI_TEXT(kShared, "Shared", nullptr) \
    WA_UI_TEXT(kExclusive, "Exclusive", nullptr) \
    WA_UI_TEXT(kSupportExact, "Exact", "精确") \
    WA_UI_TEXT(kSupportClosest, "Closest", "最接近") \
    WA_UI_TEXT(kMonitorCaptureDevice, "Monitor capture device", "Monitor Capture 设备") \
    WA_UI_TEXT(kSystemLoopbackRenderDevice, "System Loopback render device", \
        "System Loopback Render 设备") \
    WA_UI_TEXT(kApplicationLoopbackFormatReference, "Application Loopback format reference", \
        "Application Loopback 格式参考") \
    WA_UI_TEXT(kCaptureWaveformSpectrogram, "Capture waveform + spectrogram", \
        "Capture 波形 + 声谱图") \
    WA_UI_TEXT(kRenderWaveformSpectrogramDelayed, "Render waveform + spectrogram (delayed)", \
        "Render 波形 + 声谱图（延迟）") \
    WA_UI_TEXT(kCaptureChannelsShown, "Capture channels: %u / %u channels shown", \
        "Capture 声道：%u / %u 个声道已显示") \
    WA_UI_TEXT(kCaptureChannelWaveform, "Capture Ch %u waveform", "Capture Ch %u 波形") \
    WA_UI_TEXT(kCaptureChannelSpectrogram, "Capture Ch %u spectrogram", "Capture Ch %u 声谱图")

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

inline constexpr const char* kAudioCategoryLabels[] = {
    kCategoryOther,
    kCategoryCommunications,
    kCategoryMedia,
    kCategoryMovie,
    kCategoryGameChat,
    kCategorySpeech,
    kCategorySoundEffects,
    kCategoryGameMedia,
};
inline constexpr int kAudioCategoryCount =
    static_cast<int>(sizeof(kAudioCategoryLabels) / sizeof(kAudioCategoryLabels[0]));

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
    case ChannelPhrase::FrontLeft: return get(kPhraseFrontLeft);
    case ChannelPhrase::FrontRight: return get(kPhraseFrontRight);
    case ChannelPhrase::FrontCenter: return get(kPhraseFrontCenter);
    case ChannelPhrase::LowFrequency: return get(kPhraseLowFrequency);
    case ChannelPhrase::BackLeft: return get(kPhraseBackLeft);
    case ChannelPhrase::BackRight: return get(kPhraseBackRight);
    case ChannelPhrase::BackCenter: return get(kPhraseBackCenter);
    case ChannelPhrase::SideLeft: return get(kPhraseSideLeft);
    case ChannelPhrase::SideRight: return get(kPhraseSideRight);
    case ChannelPhrase::FrontLeftOfCenter: return get(kPhraseFrontLeftOfCenter);
    case ChannelPhrase::FrontRightOfCenter: return get(kPhraseFrontRightOfCenter);
    case ChannelPhrase::TopCenter: return get(kPhraseTopCenter);
    case ChannelPhrase::TopFrontLeft: return get(kPhraseTopFrontLeft);
    case ChannelPhrase::TopFrontCenter: return get(kPhraseTopFrontCenter);
    case ChannelPhrase::TopFrontRight: return get(kPhraseTopFrontRight);
    case ChannelPhrase::TopBackLeft: return get(kPhraseTopBackLeft);
    case ChannelPhrase::TopBackCenter: return get(kPhraseTopBackCenter);
    case ChannelPhrase::TopBackRight: return get(kPhraseTopBackRight);
    case ChannelPhrase::Channel1: return get(kPhraseChannel1);
    case ChannelPhrase::Channel2: return get(kPhraseChannel2);
    case ChannelPhrase::Channel3: return get(kPhraseChannel3);
    case ChannelPhrase::Channel4: return get(kPhraseChannel4);
    case ChannelPhrase::Channel5: return get(kPhraseChannel5);
    case ChannelPhrase::Channel6: return get(kPhraseChannel6);
    case ChannelPhrase::Channel7: return get(kPhraseChannel7);
    case ChannelPhrase::Channel8: return get(kPhraseChannel8);
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
    out += '\n';
    out += get(kPipelineTipProcess);
    out += row.processName;
    out += '\n';
    out += get(kPipelineTipPid);
    out += std::to_string(row.processId);
    out += '\n';
    out += get(kPipelineTipDevice);
    out += row.deviceName;
    out += '\n';
    out += get(kPipelineTipVolume);
    out += vol;
    out += '\n';
    out += get(kPipelineTipMute);
    out += get(row.sessionMute ? kPipelineMuteYes : kPipelineMuteNo);
    out += '\n';
    out += get(kPipelineTipState);
    out += row.state;
    return out;
}

inline std::string channelLayoutText(uint16_t channels, uint32_t channelMask) {
    const uint32_t mask = channelMask ? channelMask : defaultChannelMask(channels);
    const char* name = nullptr;
    if (mask == defaultChannelMask(channels)) {
        switch (channels) {
        case 1: name = get(kLayoutMono); break;
        case 2: name = get(kLayoutStereo); break;
        case 4: name = get(kLayoutQuad); break;
        case 6: name = get(kLayout51); break;
        case 8: name = get(kLayout71); break;
        default: break;
        }
    }
    char suffix[64];
    if (mask != 0) {
        std::snprintf(suffix, sizeof(suffix), get(kLayoutMask), static_cast<unsigned>(mask));
    } else {
        std::snprintf(suffix, sizeof(suffix), get(kLayoutChannelCount),
                      static_cast<unsigned>(channels));
    }
    if (!name) return std::string(suffix);
    std::string text = name;
    text += " / ";
    text += suffix;
    return text;
}

inline const char* pipelineCallLogEmptyText(bool attached, bool pumpEnabled) {
    if (!attached) return get(kPipelineCallLogEmpty);
    return get(pumpEnabled ? kPipelineCallLogWaitingPump : kPipelineCallLogWaiting);
}

inline const char* formatOriginName(FormatOrigin origin) {
    switch (origin) {
    case FormatOrigin::Mix: return get(kOriginMix);
    case FormatOrigin::Device: return get(kOriginDevice);
    case FormatOrigin::Oem: return get(kOriginOem);
    case FormatOrigin::Standard: return get(kOriginStandard);
    }
    return "-";
}

inline std::string formatOriginsLabel(FormatOrigins origins) {
    std::string text;
    auto append = [&](FormatOrigin origin) {
        if (!hasFormatOrigin(origins, origin)) return;
        if (!text.empty()) text += ", ";
        text += formatOriginName(origin);
    };
    append(FormatOrigin::Mix);
    append(FormatOrigin::Device);
    append(FormatOrigin::Oem);
    append(FormatOrigin::Standard);
    return text.empty() ? std::string("-") : text;
}

inline const char* supportLabel(SupportLevel support) {
    switch (support) {
    case SupportLevel::Exact: return get(kSupportExact);
    case SupportLevel::ClosestMatch: return get(kSupportClosest);
    default: return "-";
    }
}

} // namespace wa::ui_text
