#pragma once

#include <cstdint>
#include <string>

namespace winaudio {

struct AgoraRtcRuntimeStatus {
  bool compiled_with_rtc_support = false;
  bool runtime_available = false;
  std::wstring availability_code;
  std::wstring availability_reason;
};

struct AgoraRtcConfig {
  bool enabled = false;
  std::wstring app_id;
  std::wstring token;
  std::wstring channel_id;
  uint32_t uid = 0;
  bool publish_capture_audio = true;
  uint32_t publish_sample_rate = 48000;
  uint16_t publish_channels = 2;
};

struct AgoraRtcStats {
  bool enabled = false;
  bool joined = false;
  bool join_attempted = false;
  AgoraRtcRuntimeStatus runtime_status {};
  std::wstring connection_state;
  std::wstring channel_id;
  uint32_t uid = 0;
  uint32_t publish_sample_rate = 0;
  uint16_t publish_channels = 0;
  uint64_t pushed_frames = 0;
  uint64_t push_calls = 0;
  std::wstring last_error_stage;
  std::wstring last_error_message;
  std::wstring sdk_version;
};

std::wstring MaskAgoraToken(const std::wstring& token);

}  // namespace winaudio
