#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace winaudio {

enum AudioDeviceCapabilityFlags : uint32_t {
  kDeviceCapabilityNone = 0,
  kDeviceCapabilitySharedMode = 1u << 0,
  kDeviceCapabilityExclusiveMode = 1u << 1,
  kDeviceCapabilityEventDriven = 1u << 2,
  kDeviceCapabilityTimerDriven = 1u << 3,
  kDeviceCapabilityCallbackBuffers = 1u << 4,
};

enum class AudioBackendType {
  Wasapi,
  WaveApi,
};

enum class AudioSourceMode {
  MicrophoneCapture,
  SystemLoopback,
};

enum class AudioDirection {
  Capture,
  Render,
};

enum class AudioSampleType {
  PcmInt16,
  PcmInt24,
  PcmInt32,
  Float32,
};

enum class WasapiShareMode {
  Shared,
  Exclusive,
};

enum class WasapiDriveMode {
  EventDriven,
  TimerDriven,
};

enum class DumpFileType {
  Wav,
  RawPcm,
};

struct AudioFormatSpec {
  uint32_t sample_rate = 48000;
  uint16_t channels = 2;
  AudioSampleType sample_type = AudioSampleType::Float32;
  uint16_t bits_per_sample = 32;
  uint32_t channel_mask = 0;
  uint16_t block_align = 8;
  uint32_t avg_bytes_per_sec = 384000;

  [[nodiscard]] uint16_t bytes_per_sample() const {
    return static_cast<uint16_t>(bits_per_sample / 8);
  }

  [[nodiscard]] uint32_t frame_bytes() const {
    return static_cast<uint32_t>(channels * bytes_per_sample());
  }

  void normalize();
};

struct AudioDeviceDescriptor {
  std::wstring id;
  std::wstring friendly_name;
  bool is_default = false;
  AudioDirection direction = AudioDirection::Capture;
  bool supports_loopback = false;
  uint32_t capability_flags = 0;
};

struct CaptureConfig {
  AudioBackendType backend = AudioBackendType::Wasapi;
  AudioSourceMode source_mode = AudioSourceMode::MicrophoneCapture;
  std::wstring device_id;
  AudioFormatSpec format {};
  uint32_t buffer_duration_ms = 40;
  WasapiShareMode wasapi_share_mode = WasapiShareMode::Shared;
  WasapiDriveMode wasapi_drive_mode = WasapiDriveMode::EventDriven;
  bool dump_enabled = false;
  std::wstring dump_path;
  DumpFileType dump_file_type = DumpFileType::Wav;
};

struct RenderConfig {
  AudioBackendType backend = AudioBackendType::Wasapi;
  std::wstring device_id;
  AudioFormatSpec format {};
  bool monitor_enabled = true;
  uint32_t fixed_delay_ms = 120;
  uint32_t buffer_duration_ms = 40;
  WasapiShareMode wasapi_share_mode = WasapiShareMode::Shared;
  WasapiDriveMode wasapi_drive_mode = WasapiDriveMode::EventDriven;
};

struct AudioFrameChunk {
  AudioFormatSpec format {};
  std::vector<float> interleaved_samples;
  int64_t first_sample_qpc = 0;
  uint64_t frame_index = 0;

  [[nodiscard]] uint32_t frame_count() const {
    if (format.channels == 0) {
      return 0;
    }
    return static_cast<uint32_t>(interleaved_samples.size() / format.channels);
  }
};

struct MeterValues {
  float peak = 0.0f;
  float rms = 0.0f;
  float dbfs = -100.0f;
  bool clipping = false;
};

struct WaveformEnvelopePoint {
  float min_value = 0.0f;
  float max_value = 0.0f;
};

struct SessionRuntimeStats {
  MeterValues capture_meter {};
  MeterValues render_meter {};
  uint32_t queue_depth_ms = 0;
  uint64_t queue_depth_frames = 0;
  uint64_t dropped_frames = 0;
  uint64_t render_underruns = 0;
  uint64_t capture_overruns = 0;
  uint64_t render_wave_updates = 0;
  uint32_t estimated_monitor_delay_ms = 0;
  bool active_render_monitor_enabled = true;
  bool active_requested_timing_present = false;
  bool active_requested_wasapi_mode_present = false;
  std::wstring requested_capture_format;
  std::wstring requested_render_format;
  std::wstring requested_capture_device_id;
  std::wstring requested_render_device_id;
  uint32_t requested_monitor_delay_ms = 0;
  uint32_t requested_capture_buffer_duration_ms = 0;
  uint32_t requested_render_buffer_duration_ms = 0;
  std::wstring requested_capture_wasapi_mode;
  std::wstring requested_render_wasapi_mode;
  std::wstring negotiated_capture_format;
  std::wstring negotiated_render_format;
  std::wstring actual_capture_backend_mode;
  std::wstring actual_render_backend_mode;
  std::wstring actual_resampler_mode;
  std::wstring capture_runtime_details;
  std::wstring render_runtime_details;
  std::wstring dump_path;
  std::wstring last_device_change_reason;
  std::wstring last_device_change_result;
  std::wstring last_rebuild_reason;
  std::wstring last_rebuild_result;
  std::wstring last_error_stage;
  std::wstring last_error_message;
};

std::wstring ToWideString(AudioBackendType value);
std::wstring ToWideString(AudioSourceMode value);
std::wstring ToWideString(AudioDirection value);
std::wstring ToWideString(AudioSampleType value);
std::wstring ToWideString(WasapiShareMode value);
std::wstring ToWideString(WasapiDriveMode value);
std::wstring ToWideString(DumpFileType value);
std::wstring DescribeAudioFormat(const AudioFormatSpec& format);
std::wstring DescribeDeviceCapabilities(const AudioDeviceDescriptor& device);

}  // namespace winaudio
