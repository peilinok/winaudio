#pragma once
#include <cstdint>
namespace wa {

// Advanced WASAPI stream parameters. SetClientProperties is all-or-nothing:
// when clientProperties.enabled is false the call is skipped; when it is true
// every AudioClientProperties field is populated from this struct.
enum class AudioCategory : uint8_t { Other, Communications, Media, Movie,
                                     GameChat, Speech, SoundEffects, GameMedia };
enum class StreamOption  : uint8_t { None, Raw, MatchFormat, Ambisonics,
                                     PostVolumeLoopback };          // AUDCLNT_STREAMOPTIONS
enum class DuckingMode   : uint8_t { Default, OptOut };            // render-only
enum class AutoConvert   : uint8_t { Default, Force, Off };        // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM

struct ClientProperties {
    bool          enabled  = false;                          // false = do not call SetClientProperties
    AudioCategory category = AudioCategory::Communications;   // AudioClientProperties::eCategory
    bool          offload  = false;                          // AudioClientProperties::bIsOffload
    StreamOption  option   = StreamOption::None;             // AudioClientProperties::Options
};

struct StreamParams {
    ClientProperties clientProperties{};
    DuckingMode      ducking  = DuckingMode::Default;
    uint32_t         bufferMs = 0;                    // 0 = current behavior (Shared 100 ms / Excl minPer)
    AutoConvert      autoConvert = AutoConvert::Default;

    bool anyClientProps() const { return clientProperties.enabled; }
    bool isDefault() const {
        return !anyClientProps() && ducking == DuckingMode::Default && bufferMs == 0
            && autoConvert == AutoConvert::Default;
    }
};

} // namespace wa
