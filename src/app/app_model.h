#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "audio/audio_session_controller.h"

namespace winaudio {

class AppModel final : public ISessionEventSink {
 public:
  AppModel();
  explicit AppModel(std::unique_ptr<IAudioBackendFactory> backend_factory);

  bool Initialize();
  void RefreshDevices(bool rebuild_running_follow_defaults = false);
  bool Start();
  void Stop();
  bool Tick();
  void SetCaptureBackend(AudioBackendType backend);
  void SetRenderBackend(AudioBackendType backend);
  void SetCaptureSourceMode(AudioSourceMode source_mode);
  void SetApplicationLoopbackTarget(
      ApplicationLoopbackTargetKind target_kind,
      const std::wstring& target_value);
  void SetCaptureDeviceId(const std::wstring& device_id);
  void SetRenderDeviceId(const std::wstring& device_id);
  void SetFixedDelayMs(uint32_t delay_ms);
  void SetDumpEnabled(bool enabled);
  void SetDumpPath(const std::wstring& dump_path);
  void SetDumpFileType(DumpFileType file_type);
  void SetCaptureSampleRate(uint32_t sample_rate);
  void SetCaptureChannels(uint16_t channels);
  void SetCaptureSampleType(AudioSampleType sample_type);
  void SetRenderSampleRate(uint32_t sample_rate);
  void SetRenderChannels(uint16_t channels);
  void SetRenderSampleType(AudioSampleType sample_type);
  void SetCaptureWasapiShareMode(WasapiShareMode share_mode);
  void SetCaptureWasapiDriveMode(WasapiDriveMode drive_mode);
  void SetCaptureWasapiStreamCategory(WasapiStreamCategory category);
  void SetCaptureWasapiStreamOptions(WasapiStreamOptions options);
  void SetRenderWasapiShareMode(WasapiShareMode share_mode);
  void SetRenderWasapiDriveMode(WasapiDriveMode drive_mode);
  void SetRenderWasapiStreamCategory(WasapiStreamCategory category);
  void SetRenderWasapiStreamOptions(WasapiStreamOptions options);
  void SetCaptureBufferDurationMs(uint32_t duration_ms);
  void SetRenderBufferDurationMs(uint32_t duration_ms);
  void SetMonitorEnabled(bool enabled);
  void SetFollowDefaultDevices(bool enabled);
  void SetAutoAlignRenderFormat(bool enabled);
  void HandleDefaultDeviceRefresh();
  void RecordProbeResult(const std::wstring& stage,
                         uint32_t ticks,
                         bool capture_wave_seen,
                         bool render_wave_seen,
                         const std::wstring& dump_path,
                         uint64_t dump_bytes,
                         uint64_t render_updates,
                         const std::wstring& result,
                         const std::wstring& failure_stage = L"",
                         const std::wstring& failure_reason = L"");
  bool RunQuickProbe();
  void RecordProbeBatchResult(const std::vector<std::wstring>& lines);
  bool RunProbeMatrix();
  bool RunCaptureOpenProbe();
  bool RunProbeMatrixForSources(const std::vector<AudioSourceMode>& source_modes);
  bool RunProbeMatrixFiltered(
      const std::vector<AudioSourceMode>& source_modes,
      const std::vector<AudioBackendType>& capture_backends,
      const std::vector<AudioBackendType>& render_backends,
      const std::vector<WasapiShareMode>& wasapi_share_modes,
      const std::vector<bool>& align_modes,
      const std::vector<std::wstring>& profile_labels,
      const std::vector<std::wstring>& delay_labels = {},
      const std::vector<std::wstring>& buffer_labels = {});
  bool RunProbeMatrixForSourcesAndBackends(
      const std::vector<AudioSourceMode>& source_modes,
      const std::vector<AudioBackendType>& capture_backends,
      const std::vector<AudioBackendType>& render_backends);
  bool RunProbeMatrixForSourcesAndRenderBackends(
      const std::vector<AudioSourceMode>& source_modes,
      const std::vector<AudioBackendType>& render_backends);

  [[nodiscard]] SessionConfiguration configuration() const;
  [[nodiscard]] DeviceEnumerationSnapshot devices() const;
  [[nodiscard]] SessionRuntimeStats stats() const;
  [[nodiscard]] std::vector<WaveformEnvelopePoint> capture_waveform() const;
  [[nodiscard]] std::vector<WaveformEnvelopePoint> render_waveform() const;
  [[nodiscard]] std::vector<std::wstring> logs() const;
  [[nodiscard]] std::wstring session_state() const;
  [[nodiscard]] std::wstring summary_text() const;
  [[nodiscard]] std::wstring diagnostics_text() const;
  [[nodiscard]] std::wstring capability_text() const;
  [[nodiscard]] std::wstring probe_text() const;

  void OnLogLine(const std::wstring& line) override;
  void OnStatsUpdated(const SessionRuntimeStats& stats) override;
  void OnWaveformUpdated(AudioDirection direction,
                         const std::vector<WaveformEnvelopePoint>& waveform,
                         const MeterValues& meter) override;
  void OnDevicesUpdated(const DeviceEnumerationSnapshot& snapshot) override;
  void OnSessionStateChanged(const std::wstring& state) override;

 private:
  AppModel(std::unique_ptr<IAudioBackendFactory> backend_factory,
           bool uses_real_backend_factory);
  [[nodiscard]] bool UsesRealBackendFactory() const;
  void ClearWaveformCachesLocked();
  void RefreshCapabilitySnapshot();

  mutable std::mutex mutex_;
  AudioSessionController controller_;
  bool uses_real_backend_factory_ = true;
  SessionConfiguration configuration_ {};
  DeviceEnumerationSnapshot devices_ {};
  SessionRuntimeStats stats_ {};
  std::vector<WaveformEnvelopePoint> capture_waveform_;
  std::vector<WaveformEnvelopePoint> render_waveform_;
  std::vector<std::wstring> logs_;
  std::wstring session_state_ = L"Idle";
  bool non_loopback_monitor_preference_ = true;
  std::wstring capability_text_cache_;
  std::wstring probe_stage_;
  uint32_t probe_ticks_ = 0;
  bool probe_capture_wave_seen_ = false;
  bool probe_render_wave_seen_ = false;
  std::wstring probe_dump_path_;
  uint64_t probe_dump_bytes_ = 0;
  std::wstring probe_dump_status_;
  uint64_t probe_render_updates_ = 0;
  std::wstring probe_requested_capture_mode_;
  std::wstring probe_requested_render_mode_;
  std::wstring probe_requested_capture_device_id_;
  std::wstring probe_requested_render_device_id_;
  std::wstring probe_capture_mode_;
  std::wstring probe_render_mode_;
  std::wstring probe_requested_capture_format_;
  std::wstring probe_requested_render_format_;
  std::wstring probe_negotiated_capture_format_;
  std::wstring probe_negotiated_render_format_;
  std::wstring probe_resampler_mode_;
  std::wstring probe_capture_runtime_details_;
  std::wstring probe_render_runtime_details_;
  std::wstring probe_result_;
  std::wstring probe_failure_stage_;
  std::wstring probe_failure_reason_;
  std::wstring probe_render_wave_note_;
  std::vector<std::wstring> probe_batch_lines_;
};

}  // namespace winaudio
