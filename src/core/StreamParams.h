#pragma once
#include <cstdint>
namespace wa {

// Advanced WASAPI stream parameters. ALL defaults mean "follow system": when a field is
// Default/0 the corresponding Windows call is NOT made at all, so StreamParams{} keeps the
// open path byte-for-byte identical to the pre-StreamParams behavior.
enum class AudioCategory : uint8_t { Default, Other, Communications, Media, Movie,
                                     GameChat, Speech, SoundEffects, GameMedia };
enum class StreamOption  : uint8_t { Default, Raw, MatchFormat };  // AUDCLNT_STREAMOPTIONS
enum class OffloadMode   : uint8_t { Default, Force };             // render-only
enum class DuckingMode   : uint8_t { Default, OptOut };            // render-only

struct StreamParams {
    AudioCategory category = AudioCategory::Default;  // Default = no SetClientProperties
    StreamOption  option   = StreamOption::Default;
    OffloadMode   offload  = OffloadMode::Default;
    DuckingMode   ducking  = DuckingMode::Default;
    uint32_t      bufferMs = 0;                       // 0 = current behavior (Shared 100 ms / Excl minPer)

    bool anyClientProps() const {   // category/option/offload need IAudioClient2::SetClientProperties
        return category != AudioCategory::Default || option != StreamOption::Default
            || offload  != OffloadMode::Default;
    }
    bool isDefault() const {
        return !anyClientProps() && ducking == DuckingMode::Default && bufferMs == 0;
    }
};

} // namespace wa
