#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "audio/audio_types.h"

namespace winaudio {

class SignalAnalyzer {
 public:
  explicit SignalAnalyzer(uint32_t waveform_points_limit = 512);

  void Push(const AudioFrameChunk& chunk);
  void Reset();

  [[nodiscard]] MeterValues meter() const;
  [[nodiscard]] std::vector<WaveformEnvelopePoint> waveform() const;

 private:
  uint32_t waveform_points_limit_;
  MeterValues latest_meter_ {};
  std::deque<WaveformEnvelopePoint> waveform_points_;
};

}  // namespace winaudio
