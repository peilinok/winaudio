#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <vector>

#include "audio/audio_types.h"

namespace winaudio {

WAVEFORMATEXTENSIBLE MakeWaveFormatExtensible(const AudioFormatSpec& format);
AudioFormatSpec AudioFormatFromWaveFormat(const WAVEFORMATEX& format);

std::vector<float> ConvertPcmToFloat(const BYTE* data,
                                     uint32_t frames,
                                     const WAVEFORMATEX& format);

void ConvertFloatToPcm(const float* samples,
                       uint32_t frames,
                       const WAVEFORMATEX& format,
                       std::vector<BYTE>* output);

}  // namespace winaudio
