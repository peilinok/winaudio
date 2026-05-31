#pragma once

#include <windows.h>

#include <vector>

#include "audio/audio_types.h"

namespace winaudio {

class WaveformRenderer {
 public:
  static void Draw(HDC hdc,
                   const RECT& rect,
                   const std::vector<WaveformEnvelopePoint>& waveform,
                   COLORREF accent_color,
                   const wchar_t* label,
                   const MeterValues& meter);
};

}  // namespace winaudio
