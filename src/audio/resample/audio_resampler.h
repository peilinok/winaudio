#pragma once

#include <memory>
#include <optional>

#include "audio/audio_types.h"

namespace winaudio {

class IAudioResampler {
 public:
  virtual ~IAudioResampler() = default;
  virtual bool Configure(const AudioFormatSpec& input_format,
                         const AudioFormatSpec& output_format) = 0;
  virtual std::optional<AudioFrameChunk> Resample(
      const AudioFrameChunk& input) = 0;
  virtual std::wstring mode_name() const = 0;
};

std::unique_ptr<IAudioResampler> CreateAudioResampler();

}  // namespace winaudio
