#include "audio/rtc_sidecar.h"

#include <utility>

namespace winaudio {

RtcSidecar::RtcSidecar(AgoraRtcPublisherFactory publisher_factory)
    : publisher_factory_(std::move(publisher_factory)) {}

void RtcSidecar::Initialize() {
  if (!publisher_) {
    publisher_ = CreatePublisher();
  }
}

void RtcSidecar::Reset() {
  if (publisher_) {
    publisher_->Stop();
  }
}

bool RtcSidecar::Attach(const AgoraRtcConfig& config,
                        const AudioFormatSpec& capture_format,
                        const std::function<void(const std::wstring&)>& log) {
  Initialize();
  if (!publisher_ || capture_format.sample_rate == 0) {
    return false;
  }

  config_ = config;
  const auto runtime = publisher_->runtime_status();
  if (!runtime.runtime_available) {
    log(runtime.availability_reason);
    return false;
  }
  if (!publisher_->Initialize(config_)) {
    log(L"Failed to initialize RTC publisher.");
    return false;
  }
  if (!publisher_->Start(capture_format)) {
    log(L"Failed to start RTC publisher.");
    return false;
  }
  log(L"RTC channel join requested.");
  return true;
}

void RtcSidecar::Detach(const std::wstring& log_line,
                        const std::function<void(const std::wstring&)>& log) {
  Reset();
  if (!log_line.empty()) {
    log(log_line);
  }
}

bool RtcSidecar::Publish(const AudioFrameChunk& chunk,
                         const std::function<void(const std::wstring&)>& log) {
  if (!publisher_ || !publisher_->stats().joined) {
    return false;
  }
  if (publisher_->PublishChunk(chunk)) {
    return true;
  }
  const auto rtc_stats = publisher_->stats();
  const auto message =
      rtc_stats.last_error_message.empty()
          ? std::wstring(L"RTC publisher rejected audio chunk.")
          : rtc_stats.last_error_message;
  Reset();
  log(L"RTC sidecar disabled after publish failure: " + message);
  return false;
}

AgoraRtcStats RtcSidecar::stats() const {
  if (!publisher_) {
    AgoraRtcStats stats;
    stats.runtime_status = GetAgoraRtcRuntimeStatus();
    return stats;
  }
  return publisher_->stats();
}

AgoraRtcRuntimeStatus RtcSidecar::runtime_status() const {
  if (!publisher_) {
    return GetAgoraRtcRuntimeStatus();
  }
  return publisher_->runtime_status();
}

std::unique_ptr<AgoraRtcPublisher> RtcSidecar::CreatePublisher() const {
  if (publisher_factory_) {
    return publisher_factory_();
  }
  return CreateAgoraRtcPublisher();
}

}  // namespace winaudio
