#pragma once

#include <functional>
#include <memory>
#include <string>

#include "audio/audio_types.h"
#include "audio/resample/audio_resampler.h"
#include "rtc/agora_rtc_publisher.h"

namespace winaudio {

class RtcSidecar {
 public:
  explicit RtcSidecar(AgoraRtcPublisherFactory publisher_factory);

  void Initialize();
  void Reset();
  bool Attach(const AgoraRtcConfig& config,
              const AudioFormatSpec& capture_format,
              const std::function<void(const std::wstring&)>& log);
  void Detach(const std::wstring& log_line,
              const std::function<void(const std::wstring&)>& log);
  bool Publish(const AudioFrameChunk& chunk,
               const std::function<void(const std::wstring&)>& log);
  [[nodiscard]] AgoraRtcStats stats() const;
  [[nodiscard]] AgoraRtcRuntimeStatus runtime_status() const;

 private:
  std::unique_ptr<AgoraRtcPublisher> CreatePublisher() const;

  AgoraRtcPublisherFactory publisher_factory_;
  std::unique_ptr<AgoraRtcPublisher> publisher_;
  AgoraRtcConfig config_ {};
};

}  // namespace winaudio
