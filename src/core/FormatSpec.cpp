#include "FormatSpec.h"
#include <charconv>

namespace wa {

bool parseFormatSpec(const std::string& spec, AudioFormat& out) {
    const char* p = spec.data();
    const char* end = p + spec.size();

    unsigned rate = 0, bits = 0, ch = 0;
    // from_chars rejects a leading sign/space for unsigned and gives an end pointer,
    // so we can enforce strict structure and full-string consumption.
    auto r1 = std::from_chars(p, end, rate);
    if (r1.ec != std::errc() || r1.ptr == end || *r1.ptr != '/') return false;
    p = r1.ptr + 1;
    auto r2 = std::from_chars(p, end, bits);
    if (r2.ec != std::errc() || r2.ptr == end || *r2.ptr != '/') return false;
    p = r2.ptr + 1;
    auto r3 = std::from_chars(p, end, ch);
    if (r3.ec != std::errc()) return false;
    p = r3.ptr;

    bool isFloat = false;
    if (p != end) {
        // The only permitted trailing character is a single 'f'/'F'.
        if ((*p == 'f' || *p == 'F') && (p + 1) == end) isFloat = true;
        else return false; // trailing garbage
    }

    if (rate == 0 || rate > 768000) return false;
    if (ch == 0 || ch > 8) return false;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;

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
        {48000, 2, 32, false},  // 32-bit PCM
        {48000, 2, 32, true},   // 32-bit float
        {96000, 2, 24, false},
        {48000, 1, 16, false},
    };
}

} // namespace wa
