#pragma once

#include <memory>
#include <optional>
#include <string>

#include "audio/audio_types.h"
#include "audio/resample/audio_resampler.h"
#include "rtc/agora_rtc_types.h"

namespace winaudio {

class AgoraRtcPublisher {
 public:
  virtual ~AgoraRtcPublisher() = default;

  virtual bool Initialize(const AgoraRtcConfig& config) = 0;
  virtual bool Start(const AudioFormatSpec& capture_format) = 0;
  virtual void Stop() = 0;
  virtual bool PublishChunk(const AudioFrameChunk& chunk) = 0;
  virtual AgoraRtcStats stats() const = 0;
};

std::unique_ptr<AgoraRtcPublisher> CreateAgoraRtcPublisher();

}  // namespace winaudio
