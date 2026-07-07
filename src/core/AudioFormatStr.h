#pragma once
#include "AudioFormat.h"
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

} // namespace wa
