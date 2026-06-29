#pragma once
#include "AudioFormatDef.h"
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>
namespace wa {
WAVEFORMATEXTENSIBLE toWaveFormatExtensible(const AudioFormat& f);
AudioFormat fromWaveFormat(const WAVEFORMATEX* wf);
} // namespace wa
