#pragma once
#include <cstddef>
#include <cstdint>
#include "AudioFormat.h"

namespace wa {

/// Convert interleaved raw PCM bytes to interleaved float samples.
/// Supports bitsPerSample 16, 24, 32 (integer) and 32-bit float (isFloat).
void pcmToFloat(const uint8_t* in, size_t frames, const AudioFormat& f, float* outInterleaved);

/// Convert interleaved float samples back to interleaved raw PCM bytes.
/// Supports bitsPerSample 16, 24, 32 (integer) and 32-bit float (isFloat).
void floatToPcm(const float* inInterleaved, size_t frames, const AudioFormat& f, uint8_t* out);

/// Downmix interleaved multi-channel float buffer to mono by averaging channels.
void downmixMono(const float* interleaved, size_t frames, uint16_t ch, float* monoOut);

/// Adapt channel count: 1->2 duplicate, 2->1 average, else copy min(inCh,outCh) and zero-fill.
void adaptChannels(const float* in, uint16_t inCh, float* out, uint16_t outCh, size_t frames);

/// Return the peak absolute value of a mono (or flat) float buffer.
float peakLevel(const float* mono, size_t n);

} // namespace wa
