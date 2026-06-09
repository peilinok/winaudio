#include "rtc/agora_rtc_publisher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "rtc/agora_rtc_types.h"

#if defined(WINAUDIO_ENABLE_AGORA_SDK)
#include "IAgoraService.h"
#include "NGIAgoraAudioTrack.h"
#include "NGIAgoraLocalUser.h"
#include "NGIAgoraMediaNode.h"
#include "NGIAgoraMediaNodeFactory.h"
#include "NGIAgoraRtcConnection.h"
#endif

namespace winaudio {

namespace {

#if defined(WINAUDIO_ENABLE_AGORA_SDK)
using agora::agora_refptr;
using agora::base::AgoraServiceConfiguration;
using agora::base::IAgoraService;
using agora::rtc::AUDIO_SCENARIO_DEFAULT;
using agora::rtc::BYTES_PER_SAMPLE;
using agora::rtc::CLIENT_ROLE_BROADCASTER;
using agora::rtc::CONNECTION_CHANGED_REASON_TYPE;
using agora::rtc::ILocalAudioTrack;
using agora::rtc::ILocalUser;
using agora::rtc::IMediaNodeFactory;
using agora::rtc::IRtcConnection;
using agora::rtc::IRtcConnectionObserver;
using agora::rtc::RtcConnectionConfiguration;
using agora::rtc::TConnectionInfo;
using agora::rtc::TWO_BYTES_PER_SAMPLE;
using agora::rtc::IAudioPcmDataSender;
#endif

std::string Narrow(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr,
                      nullptr);
  return result;
}

std::wstring Widen(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  const int wide_size = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
  if (wide_size <= 1) {
    return {};
  }
  std::wstring result(static_cast<size_t>(wide_size - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), wide_size);
  return result;
}

int16_t FloatToPcm16(float sample) {
  const float clamped = std::clamp(sample, -1.0f, 1.0f);
  const float scaled =
      clamped >= 0.0f ? clamped * 32767.0f : clamped * 32768.0f;
  long rounded = lroundf(scaled);
  rounded = std::clamp<long>(rounded, std::numeric_limits<int16_t>::min(),
                             std::numeric_limits<int16_t>::max());
  return static_cast<int16_t>(rounded);
}

#if defined(WINAUDIO_ENABLE_AGORA_SDK)
class AgoraRtcConnectionObserver final : public IRtcConnectionObserver {
 public:
  explicit AgoraRtcConnectionObserver(AgoraRtcStats* stats) : stats_(stats) {}

  void onConnected(const TConnectionInfo& connectionInfo,
                   CONNECTION_CHANGED_REASON_TYPE reason) override {
    (void)reason;
    if (stats_ == nullptr) {
      return;
    }
    stats_->joined = true;
    stats_->connection_state =
        L"Connected state=" + std::to_wstring(connectionInfo.state);
  }

  void onDisconnected(const TConnectionInfo& connectionInfo,
                      CONNECTION_CHANGED_REASON_TYPE reason) override {
    if (stats_ == nullptr) {
      return;
    }
    stats_->joined = false;
    stats_->connection_state =
        L"Disconnected state=" + std::to_wstring(connectionInfo.state) +
        L" reason=" + std::to_wstring(static_cast<int>(reason));
  }

  void onConnecting(const TConnectionInfo& connectionInfo,
                    CONNECTION_CHANGED_REASON_TYPE reason) override {
    if (stats_ == nullptr) {
      return;
    }
    stats_->connection_state =
        L"Connecting state=" + std::to_wstring(connectionInfo.state) +
        L" reason=" + std::to_wstring(static_cast<int>(reason));
  }

  void onReconnecting(const TConnectionInfo& connectionInfo,
                      CONNECTION_CHANGED_REASON_TYPE reason) override {
    if (stats_ == nullptr) {
      return;
    }
    stats_->connection_state =
        L"Reconnecting state=" + std::to_wstring(connectionInfo.state) +
        L" reason=" + std::to_wstring(static_cast<int>(reason));
  }

  void onReconnected(const TConnectionInfo& connectionInfo,
                     CONNECTION_CHANGED_REASON_TYPE reason) override {
    if (stats_ == nullptr) {
      return;
    }
    stats_->joined = true;
    stats_->connection_state =
        L"Reconnected state=" + std::to_wstring(connectionInfo.state) +
        L" reason=" + std::to_wstring(static_cast<int>(reason));
  }

  void onConnectionLost(const TConnectionInfo& connectionInfo) override {
    (void)connectionInfo;
    if (stats_ != nullptr) {
      stats_->joined = false;
      stats_->connection_state = L"ConnectionLost";
    }
  }

  void onLastmileQuality(const agora::rtc::QUALITY_TYPE quality) override {
    (void)quality;
  }

  void onLastmileProbeResult(
      const agora::rtc::LastmileProbeResult& result) override {
    (void)result;
  }

  void onConnectionFailure(
      const TConnectionInfo& connectionInfo,
      CONNECTION_CHANGED_REASON_TYPE reason) override {
    if (stats_ == nullptr) {
      return;
    }
    stats_->joined = false;
    stats_->last_error_stage = L"rtc-connect";
    stats_->last_error_message =
        L"Connection failure state=" + std::to_wstring(connectionInfo.state) +
        L" reason=" + std::to_wstring(static_cast<int>(reason));
    stats_->connection_state = L"Failed";
  }

  void onTokenPrivilegeWillExpire(const char* token) override {
    if (stats_ != nullptr) {
      (void)token;
      stats_->connection_state = L"TokenPrivilegeWillExpire";
    }
  }

  void onTokenPrivilegeDidExpire() override {
    if (stats_ != nullptr) {
      stats_->connection_state = L"TokenPrivilegeDidExpire";
    }
  }

  void onUserJoined(agora::user_id_t userId) override { (void)userId; }
  void onUserLeft(agora::user_id_t userId,
                  agora::rtc::USER_OFFLINE_REASON_TYPE reason) override {
    (void)userId;
    (void)reason;
  }

  void onTransportStats(const agora::rtc::RtcStats& stats) override { (void)stats; }

  void onChangeRoleSuccess(agora::rtc::CLIENT_ROLE_TYPE oldRole,
                           agora::rtc::CLIENT_ROLE_TYPE newRole,
                           const agora::rtc::ClientRoleOptions& newRoleOptions) override {
    (void)oldRole;
    (void)newRoleOptions;
    if (stats_ != nullptr) {
      stats_->connection_state =
          L"RoleChanged new=" + std::to_wstring(static_cast<int>(newRole));
    }
  }

  void onChangeRoleFailure(agora::rtc::CLIENT_ROLE_CHANGE_FAILED_REASON reason,
                           agora::rtc::CLIENT_ROLE_TYPE currentRole) override {
    if (stats_ != nullptr) {
      stats_->last_error_stage = L"rtc-role";
      stats_->last_error_message =
          L"Role change failed reason=" +
          std::to_wstring(static_cast<int>(reason)) + L" role=" +
          std::to_wstring(static_cast<int>(currentRole));
    }
  }

  void onChannelMediaRelayStateChanged(int state, int code) override {
    (void)state;
    (void)code;
  }

 private:
  AgoraRtcStats* stats_ = nullptr;
};

class RealAgoraRtcPublisher final : public AgoraRtcPublisher {
 public:
  RealAgoraRtcPublisher() = default;
  ~RealAgoraRtcPublisher() override { Stop(); }

  bool Initialize(const AgoraRtcConfig& config) override {
    config_ = config;
    stats_ = {};
    stats_.enabled = config.enabled;
    stats_.channel_id = config.channel_id;
    stats_.uid = config.uid;
    stats_.publish_sample_rate = config.publish_sample_rate;
    stats_.publish_channels = config.publish_channels;
    return true;
  }

  bool Start(const AudioFormatSpec& capture_format) override {
    Stop();
    if (!config_.enabled || !config_.publish_capture_audio) {
      return true;
    }

    if (config_.app_id.empty()) {
      SetError(L"rtc-init", L"Agora App ID is required.");
      return false;
    }
    if (config_.channel_id.empty()) {
      SetError(L"rtc-init", L"Agora channel id is required.");
      return false;
    }

    publish_format_.sample_rate = config_.publish_sample_rate;
    publish_format_.channels = config_.publish_channels;
    publish_format_.sample_type = AudioSampleType::PcmInt16;
    publish_format_.normalize();

    resampler_ = CreateAudioResampler();
    if (!resampler_ ||
        !resampler_->Configure(capture_format, publish_format_)) {
      SetError(L"rtc-resampler", L"Failed to configure Agora publish resampler.");
      return false;
    }

    service_ = ::createAgoraService();
    if (service_ == nullptr) {
      SetError(L"rtc-init", L"createAgoraService returned null.");
      return false;
    }

    AgoraServiceConfiguration service_config;
    const auto app_id_utf8 = Narrow(config_.app_id);
    service_config.appId = app_id_utf8.c_str();
    service_config.enableAudioProcessor = true;
    service_config.enableAudioDevice = false;
    service_config.enableVideo = false;
    service_config.channelProfile = agora::CHANNEL_PROFILE_LIVE_BROADCASTING;
    service_config.audioScenario = AUDIO_SCENARIO_DEFAULT;
    const int init_result = service_->initialize(service_config);
    if (init_result != 0) {
      SetError(L"rtc-init",
               L"IAgoraService::initialize failed: " +
                   std::to_wstring(init_result));
      service_->release();
      service_ = nullptr;
      return false;
    }

    int sdk_build = 0;
    const char* sdk_version = ::getAgoraSdkVersion(&sdk_build);
    if (sdk_version != nullptr) {
      stats_.sdk_version =
          Widen(sdk_version) + L" build=" + std::to_wstring(sdk_build);
    }

    media_node_factory_ = service_->createMediaNodeFactory();
    if (!media_node_factory_) {
      SetError(L"rtc-init", L"Failed to create IMediaNodeFactory.");
      Stop();
      return false;
    }

    pcm_sender_ = media_node_factory_->createAudioPcmDataSender();
    if (!pcm_sender_) {
      SetError(L"rtc-init", L"Failed to create IAudioPcmDataSender.");
      Stop();
      return false;
    }

    local_audio_track_ = service_->createDirectCustomAudioTrack(pcm_sender_);
    if (!local_audio_track_) {
      SetError(L"rtc-init", L"Failed to create direct custom audio track.");
      Stop();
      return false;
    }
    local_audio_track_->setEnabled(true);

    RtcConnectionConfiguration connection_config;
    connection_config.autoSubscribeAudio = false;
    connection_config.autoSubscribeVideo = false;
    connection_config.enableAudioRecordingOrPlayout = false;
    connection_config.clientRoleType = CLIENT_ROLE_BROADCASTER;
    connection_config.channelProfile = agora::CHANNEL_PROFILE_LIVE_BROADCASTING;
    connection_ = service_->createRtcConnection(connection_config);
    if (!connection_) {
      SetError(L"rtc-init", L"Failed to create IRtcConnection.");
      Stop();
      return false;
    }

    observer_ = std::make_unique<AgoraRtcConnectionObserver>(&stats_);
    if (connection_->registerObserver(observer_.get()) != 0) {
      SetError(L"rtc-init", L"Failed to register IRtcConnectionObserver.");
      Stop();
      return false;
    }

    local_user_ = connection_->getLocalUser();
    if (local_user_ == nullptr) {
      SetError(L"rtc-init", L"Failed to get ILocalUser.");
      Stop();
      return false;
    }
    local_user_->setUserRole(CLIENT_ROLE_BROADCASTER);

    const int publish_result = local_user_->publishAudio(local_audio_track_);
    if (publish_result != 0) {
      SetError(L"rtc-publish",
               L"ILocalUser::publishAudio failed: " +
                   std::to_wstring(publish_result));
      Stop();
      return false;
    }

    const auto token_utf8 = Narrow(config_.token);
    const auto channel_utf8 = Narrow(config_.channel_id);
    const auto uid_utf8 = std::to_string(config_.uid);
    stats_.join_attempted = true;
    const int connect_result =
        connection_->connect(token_utf8.empty() ? nullptr : token_utf8.c_str(),
                             channel_utf8.c_str(), uid_utf8.c_str());
    if (connect_result != 0) {
      SetError(L"rtc-connect",
               L"IRtcConnection::connect failed: " +
                   std::to_wstring(connect_result));
      Stop();
      return false;
    }

    started_ = true;
    stats_.joined = true;
    stats_.connection_state = L"Connecting";
    return true;
  }

  void Stop() override {
    started_ = false;
    if (stats_.enabled) {
      stats_.joined = false;
      stats_.connection_state = L"Left";
    }
    pcm16_buffer_.clear();
    if (local_user_ != nullptr && local_audio_track_) {
      local_user_->unpublishAudio(local_audio_track_);
    }
    local_user_ = nullptr;
    if (connection_) {
      if (observer_) {
        connection_->unregisterObserver(observer_.get());
      }
      connection_->disconnect();
      connection_.reset();
    }
    observer_.reset();
    local_audio_track_.reset();
    pcm_sender_.reset();
    media_node_factory_.reset();
    if (service_ != nullptr) {
      service_->release();
      service_ = nullptr;
    }
    resampler_.reset();
  }

  bool PublishChunk(const AudioFrameChunk& chunk) override {
    if (!started_ || !pcm_sender_) {
      return false;
    }

    auto publish_chunk = resampler_ ? resampler_->Resample(chunk)
                                    : std::optional<AudioFrameChunk>{chunk};
    if (!publish_chunk.has_value()) {
      SetError(L"rtc-publish", L"Resampler failed before sendAudioPcmData.");
      return false;
    }
    if (publish_chunk->frame_count() == 0) {
      return true;
    }

    pcm16_buffer_.resize(publish_chunk->interleaved_samples.size());
    for (size_t index = 0; index < publish_chunk->interleaved_samples.size();
         ++index) {
      pcm16_buffer_[index] = FloatToPcm16(publish_chunk->interleaved_samples[index]);
    }

    const int result = pcm_sender_->sendAudioPcmData(
        pcm16_buffer_.data(), 0, 0,
        publish_chunk->frame_count(), TWO_BYTES_PER_SAMPLE,
        publish_format_.channels, publish_format_.sample_rate);
    stats_.push_calls += 1;
    if (result != 0) {
      SetError(L"rtc-publish",
               L"IAudioPcmDataSender::sendAudioPcmData failed: " +
                   std::to_wstring(result));
      return false;
    }
    stats_.pushed_frames += publish_chunk->frame_count();
    return true;
  }

  AgoraRtcStats stats() const override { return stats_; }

 private:
  void SetError(const std::wstring& stage, const std::wstring& message) {
    stats_.last_error_stage = stage;
    stats_.last_error_message = message;
  }

  AgoraRtcConfig config_ {};
  AgoraRtcStats stats_ {};
  std::unique_ptr<AgoraRtcConnectionObserver> observer_;
  std::unique_ptr<IAudioResampler> resampler_;
  AudioFormatSpec publish_format_ {};
  IAgoraService* service_ = nullptr;
  agora_refptr<IMediaNodeFactory> media_node_factory_;
  agora_refptr<IAudioPcmDataSender> pcm_sender_;
  agora_refptr<ILocalAudioTrack> local_audio_track_;
  agora_refptr<IRtcConnection> connection_;
  ILocalUser* local_user_ = nullptr;
  bool started_ = false;
  std::vector<int16_t> pcm16_buffer_;
};
#endif

class StubAgoraRtcPublisher final : public AgoraRtcPublisher {
 public:
  bool Initialize(const AgoraRtcConfig& config) override {
    config_ = config;
    stats_ = {};
    stats_.enabled = config.enabled;
    stats_.channel_id = config.channel_id;
    stats_.uid = config.uid;
    stats_.publish_sample_rate = config.publish_sample_rate;
    stats_.publish_channels = config.publish_channels;
    return true;
  }

  bool Start(const AudioFormatSpec& capture_format) override {
    (void)capture_format;
    started_ = config_.enabled;
    stats_.joined = started_;
    stats_.join_attempted = started_;
    stats_.connection_state = started_ ? L"StubJoined" : L"Disabled";
    return true;
  }

  void Stop() override {
    started_ = false;
    if (stats_.enabled) {
      stats_.joined = false;
      stats_.connection_state = L"Left";
    }
  }

  bool PublishChunk(const AudioFrameChunk& chunk) override {
    if (!started_) {
      return false;
    }
    stats_.push_calls += 1;
    stats_.pushed_frames += chunk.frame_count();
    return true;
  }

  AgoraRtcStats stats() const override { return stats_; }

 private:
  AgoraRtcConfig config_ {};
  AgoraRtcStats stats_ {};
  bool started_ = false;
};

}  // namespace

std::unique_ptr<AgoraRtcPublisher> CreateAgoraRtcPublisher() {
#if defined(WINAUDIO_ENABLE_AGORA_SDK)
  return std::make_unique<RealAgoraRtcPublisher>();
#else
  return std::make_unique<StubAgoraRtcPublisher>();
#endif
}

}  // namespace winaudio
