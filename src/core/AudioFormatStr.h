#pragma once
#include "AudioFormat.h"
#include <cstdio>
#include <string>

namespace wa {

// "48000/16/2" (integer PCM) or "48000/32f/2" (float). A free function rather
// than a member so the vendored AudioFormat struct stays untouched; used by log
// arguments and CLI/GUI display so the format string is written once.
inline std::string formatAudio(const AudioFormat& f) {
    return std::to_string(f.sampleRate) + "/" + std::to_string(f.bitsPerSample) +
           (f.isFloat ? "f" : "") + "/" + std::to_string(f.channels);
}

// Narrow a wide device id/name to ASCII for log arguments. The explicit per-char
// cast avoids the C4244 that std::string(w.begin(), w.end()) trips under /W4.
inline std::string narrowAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// Layout name only: Mono, Stereo, Quad, 5.1, or 7.1 plus the mask when the mask
// matches the default for that count; otherwise the mask; "N channels" when empty.
// An empty mask uses the default mask for the friendly name, matching the caps UI.
inline std::string channelLayoutLabel(const AudioFormat& fmt) {
    const uint32_t mask = fmt.channelMask ? fmt.channelMask
                                          : defaultChannelMask(fmt.channels);
    const char* name = nullptr;
    if (mask == defaultChannelMask(fmt.channels)) {
        switch (fmt.channels) {
        case 1: name = "Mono"; break;
        case 2: name = "Stereo"; break;
        case 4: name = "Quad"; break;
        case 6: name = "5.1"; break;
        case 8: name = "7.1"; break;
        default: break;
        }
    }
    char suffix[32]{};
    if (mask != 0)
        std::snprintf(suffix, sizeof(suffix), "mask 0x%X", static_cast<unsigned>(mask));
    else
        std::snprintf(suffix, sizeof(suffix), "%u channels",
                      static_cast<unsigned>(fmt.channels));
    return name ? std::string(name) + " / " + suffix : std::string(suffix);
}

} // namespace wa
