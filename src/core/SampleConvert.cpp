// SampleConvert.cpp - PCM<->float conversion, downmix, channel adapt, peak level
// /W4 notes:
//   - int24 sign-extend uses arithmetic right-shift on int32_t (implementation-defined
//     but universally correct on MSVC x64; the Int24NegativeSignExtend test guards correctness).
//   - std::min avoided to prevent collision with the Windows SDK min() macro (from windows.h
//     pulled in by AudioFormat.h); ternary used instead.
//   - Explicit casts throughout to silence narrowing/signed-unsigned warnings.
#include "SampleConvert.h"
#include <algorithm>  // std::clamp
#include <cmath>      // std::lround, std::fabs
#include <cstring>    // memcpy

namespace wa {

// ---------------------------------------------------------------------------
// pcmToFloat
// ---------------------------------------------------------------------------
void pcmToFloat(const uint8_t* in, size_t frames, const AudioFormat& f, float* outInterleaved)
{
    const size_t samples = frames * static_cast<size_t>(f.channels);

    if (f.isFloat && f.bitsPerSample == 32) {
        // Raw IEEE-754 float -- copy directly.
        std::memcpy(outInterleaved, in, samples * sizeof(float));
        return;
    }

    const uint8_t* p = in;
    for (size_t i = 0; i < samples; ++i) {
        if (f.bitsPerSample == 16) {
            // Little-endian int16
            int16_t v;
            std::memcpy(&v, p, 2);
            outInterleaved[i] = static_cast<float>(v) / 32768.f;
            p += 2;
        } else if (f.bitsPerSample == 24) {
            // Little-endian int24: read 3 bytes, sign-extend via left/right shift.
            uint32_t u = static_cast<uint32_t>(p[0])
                       | (static_cast<uint32_t>(p[1]) << 8)
                       | (static_cast<uint32_t>(p[2]) << 16);
            // Shift up to fill the sign bit, then arithmetic right-shift back.
            int32_t s = static_cast<int32_t>(u << 8) >> 8;
            outInterleaved[i] = static_cast<float>(s) / 8388608.f;
            p += 3;
        } else if (f.bitsPerSample == 32) {
            // Little-endian int32
            int32_t v;
            std::memcpy(&v, p, 4);
            outInterleaved[i] = static_cast<float>(v) / 2147483648.0f;
            p += 4;
        } else {
            // Unknown format -- output silence and advance by bytes-per-sample.
            outInterleaved[i] = 0.f;
            p += f.bitsPerSample / 8u;
        }
    }
}

// ---------------------------------------------------------------------------
// floatToPcm
// ---------------------------------------------------------------------------
void floatToPcm(const float* inInterleaved, size_t frames, const AudioFormat& f, uint8_t* out)
{
    const size_t samples = frames * static_cast<size_t>(f.channels);

    if (f.isFloat && f.bitsPerSample == 32) {
        std::memcpy(out, inInterleaved, samples * sizeof(float));
        return;
    }

    uint8_t* p = out;
    for (size_t i = 0; i < samples; ++i) {
        const float x = inInterleaved[i];

        if (f.bitsPerSample == 16) {
            // Scale, round to nearest, clamp to int16 range -- no UB on overflow.
            int32_t s = static_cast<int32_t>(std::lround(x * 32768.f));
            s = std::clamp(s, -32768, 32767);
            auto v = static_cast<int16_t>(s);
            std::memcpy(p, &v, 2);
            p += 2;
        } else if (f.bitsPerSample == 24) {
            int32_t s = static_cast<int32_t>(std::lround(x * 8388608.f));
            s = std::clamp(s, -8388608, 8388607);
            // Store 3 little-endian bytes.
            p[0] = static_cast<uint8_t>(s & 0xFF);
            p[1] = static_cast<uint8_t>((s >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((s >> 16) & 0xFF);
            p += 3;
        } else if (f.bitsPerSample == 32) {
            int64_t s = static_cast<int64_t>(
                std::llround(static_cast<double>(x) * 2147483648.0));
            s = std::clamp(s,
                static_cast<int64_t>(-2147483648LL),
                static_cast<int64_t>( 2147483647LL));
            auto v = static_cast<int32_t>(s);
            std::memcpy(p, &v, 4);
            p += 4;
        } else {
            // Unknown format -- write zeros.
            const uint16_t bytesPerSample = static_cast<uint16_t>(f.bitsPerSample / 8u);
            for (uint16_t b = 0; b < bytesPerSample; ++b) {
                p[b] = 0;
            }
            p += bytesPerSample;
        }
    }
}

// ---------------------------------------------------------------------------
// downmixMono
// ---------------------------------------------------------------------------
void downmixMono(const float* interleaved, size_t frames, uint16_t ch, float* monoOut)
{
    const float invCh = 1.f / static_cast<float>(ch);
    for (size_t fr = 0; fr < frames; ++fr) {
        float sum = 0.f;
        for (uint16_t c = 0; c < ch; ++c) {
            sum += interleaved[fr * static_cast<size_t>(ch) + c];
        }
        monoOut[fr] = sum * invCh;
    }
}

// ---------------------------------------------------------------------------
// adaptChannels
// ---------------------------------------------------------------------------
void adaptChannels(const float* in, uint16_t inCh, float* out, uint16_t outCh, size_t frames)
{
    if (inCh == 1 && outCh == 2) {
        // Mono -> stereo: duplicate.
        for (size_t fr = 0; fr < frames; ++fr) {
            const float v = in[fr];
            out[fr * 2]     = v;
            out[fr * 2 + 1] = v;
        }
    } else if (inCh == 2 && outCh == 1) {
        // Stereo -> mono: average.
        for (size_t fr = 0; fr < frames; ++fr) {
            out[fr] = (in[fr * 2] + in[fr * 2 + 1]) * 0.5f;
        }
    } else {
        // General case: copy min(inCh, outCh) channels; zero-fill remainder.
        // Ternary avoids std::min collision with the Windows SDK min() macro.
        const uint16_t copyCount = (inCh < outCh) ? inCh : outCh;
        for (size_t fr = 0; fr < frames; ++fr) {
            const float* src = in  + fr * static_cast<size_t>(inCh);
            float*       dst = out + fr * static_cast<size_t>(outCh);
            for (uint16_t c = 0; c < copyCount; ++c) {
                dst[c] = src[c];
            }
            for (uint16_t c = copyCount; c < outCh; ++c) {
                dst[c] = 0.f;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// peakLevel
// ---------------------------------------------------------------------------
float peakLevel(const float* mono, size_t n)
{
    float peak = 0.f;
    for (size_t i = 0; i < n; ++i) {
        const float a = std::fabs(mono[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

} // namespace wa
