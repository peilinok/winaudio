#include "signal_analyzer.h"

#include <algorithm>
#include <cmath>

namespace winaudio {

namespace {

constexpr float kDbFloor = -100.0f;

float SafeDb(float value) {
  if (value <= 0.000001f) {
    return kDbFloor;
  }
  return 20.0f * std::log10(value);
}

}  // namespace

SignalAnalyzer::SignalAnalyzer(uint32_t waveform_points_limit)
    : waveform_points_limit_(waveform_points_limit) {}

void SignalAnalyzer::Push(const AudioFrameChunk& chunk) {
  if (chunk.interleaved_samples.empty()) {
    latest_meter_ = {};
    latest_meter_.dbfs = kDbFloor;
    return;
  }

  float peak = 0.0f;
  double square_sum = 0.0;
  float min_value = 1.0f;
  float max_value = -1.0f;

  for (float sample : chunk.interleaved_samples) {
    peak = std::max(peak, std::abs(sample));
    square_sum += static_cast<double>(sample) * sample;
    min_value = std::min(min_value, sample);
    max_value = std::max(max_value, sample);
  }

  const auto rms = static_cast<float>(
      std::sqrt(square_sum / static_cast<double>(chunk.interleaved_samples.size())));

  latest_meter_.peak = peak;
  latest_meter_.rms = rms;
  latest_meter_.dbfs = SafeDb(peak);
  latest_meter_.clipping = peak >= 0.999f;

  waveform_points_.push_back({min_value, max_value});
  while (waveform_points_.size() > waveform_points_limit_) {
    waveform_points_.pop_front();
  }
}

void SignalAnalyzer::Reset() {
  latest_meter_ = {};
  latest_meter_.dbfs = kDbFloor;
  waveform_points_.clear();
}

MeterValues SignalAnalyzer::meter() const {
  return latest_meter_;
}

std::vector<WaveformEnvelopePoint> SignalAnalyzer::waveform() const {
  return {waveform_points_.begin(), waveform_points_.end()};
}

}  // namespace winaudio
