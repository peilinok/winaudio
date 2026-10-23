#pragma once
#include <cstdint>
#include <guiddef.h>
#include <string>
#include <vector>
#include "IAudioBackend.h"
#include "Result.h"
#include "StreamParams.h"

struct IAudioClient;

namespace wa {

// Stream-level effect state from IAudioEffectsManager (Win11 build 22000+).
enum class AudioEffectState : uint8_t {
    Unknown = 0,
    Off,
    On,
};

inline const char* audioEffectStateName(AudioEffectState s) {
    switch (s) {
    case AudioEffectState::Off: return "off";
    case AudioEffectState::On:  return "on";
    default:                    return "unknown";
    }
}

// Map AUDIO_EFFECT_STATE (0=off, 1=on). Unknown for any other value.
inline AudioEffectState audioEffectStateFromWin32(uint32_t state) {
    if (state == 0) return AudioEffectState::Off;
    if (state == 1) return AudioEffectState::On;
    return AudioEffectState::Unknown;
}

// One advertised stream effect (GUID + canSetState + state). Distinct from
// endpoint SysFx / APO CLSID slots on DeviceInfo.
struct StreamAudioEffect {
    std::string id;       // braced GUID
    std::string typeName; // known AUDIO_EFFECT_TYPE_* label, else id
    bool canSetState = false;
    AudioEffectState state = AudioEffectState::Unknown;
};

// Pure mapping; no COM. Unknown GUIDs return the braced string.
std::string audioEffectTypeName(const GUID& id);
std::string guidToBracedString(const GUID& id);

// Read effects from an already-Initialize'd Shared IAudioClient via
// GetService(IAudioEffectsManager). Empty out + Ok when the interface is
// unavailable (Win10 / older SDK compile / E_NOINTERFACE). Never Starts.
Result readAudioEffectsFromClient(IAudioClient* client, std::vector<StreamAudioEffect>& out);

// Short Shared probe: Activate + Initialize (no Start) + GetAudioEffects + close.
// Does not touch DeviceEnumerator::enumerate. Optional params default to all-default.
Result probeStreamAudioEffects(DataFlow flow, const DeviceId& id,
                               std::vector<StreamAudioEffect>& out,
                               const StreamParams& params = {});

}  // namespace wa
