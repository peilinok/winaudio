#pragma once
#include <cstdint>

namespace wa {
enum class MonitorMethod : uint8_t {
    Activate, Initialize, Start, Stop, Reset, SetEventHandle, GetService, GetBufferSize,
    SetClientProperties, InitializeSharedAudioStream, CaptureGetBuffer, CaptureReleaseBuffer,
    RenderGetBuffer, RenderReleaseBuffer, SetMasterVolume, SetMute, GetMasterVolume, GetMute,
    GetState, GetFrequency, SetChannelVolume, GetChannelCount, GetChannelVolume,
    SetAudioEffectState, GetAudioEffects, Count
};
struct MonitorMethodInfo { const char* iface; const char* method; bool control; };
inline constexpr MonitorMethodInfo kMonitorMethods[] = {
    {"IMMDevice", "Activate", true},
    {"IAudioClient", "Initialize", true}, {"IAudioClient", "Start", true},
    {"IAudioClient", "Stop", true}, {"IAudioClient", "Reset", true},
    {"IAudioClient", "SetEventHandle", true}, {"IAudioClient", "GetService", true},
    {"IAudioClient", "GetBufferSize", false},
    {"IAudioClient2", "SetClientProperties", true},
    {"IAudioClient3", "InitializeSharedAudioStream", true},
    {"IAudioCaptureClient", "GetBuffer", false}, {"IAudioCaptureClient", "ReleaseBuffer", false},
    {"IAudioRenderClient", "GetBuffer", false}, {"IAudioRenderClient", "ReleaseBuffer", false},
    {"ISimpleAudioVolume", "SetMasterVolume", true}, {"ISimpleAudioVolume", "SetMute", true},
    {"ISimpleAudioVolume", "GetMasterVolume", false}, {"ISimpleAudioVolume", "GetMute", false},
    {"IAudioSessionControl", "GetState", false}, {"IAudioClock", "GetFrequency", false},
    {"IAudioStreamVolume", "SetChannelVolume", true}, {"IAudioStreamVolume", "GetChannelCount", false},
    {"IAudioStreamVolume", "GetChannelVolume", false},
    {"IAudioEffectsManager", "SetAudioEffectState", true}, {"IAudioEffectsManager", "GetAudioEffects", false}
};
inline constexpr uint32_t methodBit(MonitorMethod method) { return 1u << static_cast<unsigned>(method); }
inline constexpr uint32_t kAllMonitorMethods = (1u << static_cast<unsigned>(MonitorMethod::Count)) - 1u;
inline constexpr uint32_t defaultMonitorMethods() {
    uint32_t mask = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(MonitorMethod::Count); ++i)
        if (kMonitorMethods[i].control) mask |= 1u << i;
    return mask;
}
inline constexpr uint32_t kDefaultMonitorMethods = defaultMonitorMethods();
}
