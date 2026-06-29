#define _CRT_SECURE_NO_WARNINGS
#include "FormatSpec.h"
#include <cstdio>
#include <cstdlib>

namespace wa {

bool parseFormatSpec(const std::string& spec, AudioFormat& out) {
    // <rate>/<bits>/<ch> with optional trailing 'f' on bits meaning float.
    unsigned rate = 0, bits = 0, ch = 0;
    char tail = 0;
    // sscanf returns the count of matched fields; require rate/bits/ch.
    int n = std::sscanf(spec.c_str(), "%u/%u/%u%c", &rate, &bits, &ch, &tail);
    if (n < 3) return false;
    bool isFloat = false;
    if (n == 4) {
        if (tail == 'f' || tail == 'F') isFloat = true;
        else return false; // any other trailing char is malformed
    }
    if (rate == 0 || ch == 0 || (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return false;
    out.sampleRate = rate;
    out.bitsPerSample = static_cast<uint16_t>(bits);
    out.channels = static_cast<uint16_t>(ch);
    out.isFloat = isFloat;
    return true;
}

long long alignedBufferDuration100ns(uint32_t sampleRate, uint32_t alignedFrames) {
    if (sampleRate == 0) return 0;
    // MSDN exclusive-mode realignment: hns = 10000 * 1000 / rate * frames + 0.5
    return static_cast<long long>(10000.0 * 1000.0 / sampleRate * alignedFrames + 0.5);
}

int selectSupportedFormat(const std::vector<AudioFormat>& candidates,
                          const std::function<bool(const AudioFormat&)>& isSupported) {
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
        if (isSupported(candidates[i])) return i;
    return -1;
}

std::vector<AudioFormat> defaultExclusiveCaptureCandidates() {
    return {
        {48000, 2, 16, false},
        {44100, 2, 16, false},
        {48000, 2, 24, false},
        {48000, 2, 32, true},
        {48000, 1, 16, false},
    };
}

} // namespace wa
