#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "AudioFormatDef.h"
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>
namespace wa {
uint32_t defaultChannelMask(uint16_t channels);
WAVEFORMATEXTENSIBLE toWaveFormatExtensible(const AudioFormat& f);
AudioFormat fromWaveFormat(const WAVEFORMATEX* wf);
} // namespace wa
