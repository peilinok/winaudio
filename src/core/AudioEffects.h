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

// Map On/Off to AUDIO_EFFECT_STATE. Unknown is not valid for Set.
inline bool audioEffectStateToWin32(AudioEffectState s, uint32_t& out) {
    if (s == AudioEffectState::Off) { out = 0; return true; }
    if (s == AudioEffectState::On)  { out = 1; return true; }
    return false;
}

// Parse "on" / "off" (case-insensitive). Unknown if neither.
inline AudioEffectState audioEffectStateFromName(const std::string& name) {
    if (name.size() == 2 &&
        (name[0] == 'o' || name[0] == 'O') &&
        (name[1] == 'n' || name[1] == 'N')) {
        return AudioEffectState::On;
    }
    if (name.size() == 3 &&
        (name[0] == 'o' || name[0] == 'O') &&
        (name[1] == 'f' || name[1] == 'F') &&
        (name[2] == 'f' || name[2] == 'F')) {
        return AudioEffectState::Off;
    }
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

// Parse "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (braces optional).
bool parseGuidBraced(const std::string& s, GUID& out);

// Resolve token as braced GUID, known type name, or kebab slug
// (e.g. "noise-suppression"). Case-insensitive for names/slugs.
bool resolveAudioEffectId(const std::string& token, GUID& out);

// Read effects from an already-Initialize'd Shared IAudioClient via
// GetService(IAudioEffectsManager). Empty out + Ok when the interface is
// unavailable (Win10 / older SDK compile / E_NOINTERFACE). Never Starts.
Result readAudioEffectsFromClient(IAudioClient* client, std::vector<StreamAudioEffect>& out);

// Set one effect on an already-Initialize'd Shared client. Verifies the
// effect is present and canSetState before calling SetAudioEffectState.
// Fails cleanly when IAudioEffectsManager is missing (Win10) or read-only.
Result setAudioEffectStateOnClient(IAudioClient* client, const GUID& effectId,
                                   AudioEffectState state);

// Short Shared probe: Activate + Initialize (no Start) + GetAudioEffects + close.
// Does not touch DeviceEnumerator::enumerate. Optional params default to all-default.
Result probeStreamAudioEffects(DataFlow flow, const DeviceId& id,
                               std::vector<StreamAudioEffect>& out,
                               const StreamParams& params = {});

// Short Shared probe: Initialize + SetAudioEffectState + optional re-Get + close.
// outEffects may be null. Token form accepts GUID or type name / kebab slug.
Result setStreamAudioEffect(DataFlow flow, const DeviceId& id,
                            const GUID& effectId, AudioEffectState state,
                            std::vector<StreamAudioEffect>* outEffects = nullptr,
                            const StreamParams& params = {});
Result setStreamAudioEffect(DataFlow flow, const DeviceId& id,
                            const std::string& effectToken, AudioEffectState state,
                            std::vector<StreamAudioEffect>* outEffects = nullptr,
                            const StreamParams& params = {});

}  // namespace wa
