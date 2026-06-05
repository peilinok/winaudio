#include "app_model.h"

#include "audio/backends/real_backends.h"
#include "probe_ui_text.h"

#include <chrono>
#include <map>
#include <thread>

namespace winaudio {

namespace {

std::wstring DescribeDumpStatus(uint64_t dump_bytes) {
  if (dump_bytes == 0) {
    return L"none";
  }
  return dump_bytes <= 44 ? L"header-only" : L"data";
}

struct MatrixBucketSummary {
  int pass = 0;
  int fail = 0;
  int capture_missing = 0;
  int render_missing = 0;
  int tick_fail = 0;
};

}  // namespace

AppModel::AppModel()
    : AppModel(std::make_unique<RealAudioBackendFactory>(), true) {}

AppModel::AppModel(std::unique_ptr<IAudioBackendFactory> backend_factory)
    : AppModel(std::move(backend_factory), false) {}

AppModel::AppModel(std::unique_ptr<IAudioBackendFactory> backend_factory,
                   bool uses_real_backend_factory)
    : controller_(std::move(backend_factory)),
      uses_real_backend_factory_(uses_real_backend_factory) {
  configuration_.capture.backend = AudioBackendType::Wasapi;
  configuration_.capture.source_mode = AudioSourceMode::MicrophoneCapture;
  configuration_.capture.dump_enabled = false;
  configuration_.capture.format.normalize();
  configuration_.render.backend = AudioBackendType::Wasapi;
  configuration_.render.monitor_enabled = true;
  non_loopback_monitor_preference_ = true;
  configuration_.render.fixed_delay_ms = 120;
  configuration_.render.format.normalize();
}

bool AppModel::Initialize() {
  if (!controller_.Initialize()) {
    return false;
  }
  RefreshDevices();
  RefreshCapabilitySnapshot();
  return true;
}

bool AppModel::UsesRealBackendFactory() const {
  return uses_real_backend_factory_;
}

void AppModel::RefreshDevices(bool rebuild_running_follow_defaults) {
  SessionConfiguration config_copy;
  bool should_restart = false;
  {
    std::scoped_lock lock(mutex_);
    should_restart = session_state_ == L"Running";
    if (configuration_.follow_default_devices) {
      configuration_.capture.device_id.clear();
      configuration_.render.device_id.clear();
    }
    config_copy = configuration_;
  }
  if (should_restart && rebuild_running_follow_defaults &&
      config_copy.follow_default_devices) {
    controller_.Stop();
    OnLogLine(L"Refresh requested while default-tracked session is running. Rebuilding active session.");
    {
      std::scoped_lock lock(mutex_);
      stats_.last_device_change_reason = L"refresh-devices";
      stats_.last_device_change_result = L"rebuilding-active-session";
      stats_.last_rebuild_reason = L"refresh-devices";
      stats_.last_rebuild_result = L"restarting";
    }
  }
  const auto snapshot = controller_.RefreshDevices(config_copy);
  OnDevicesUpdated(snapshot);
  if (should_restart && rebuild_running_follow_defaults &&
      config_copy.follow_default_devices) {
    if (controller_.Start(config_copy, this)) {
      std::scoped_lock lock(mutex_);
      stats_.last_device_change_result = L"rebuild-success";
      stats_.last_rebuild_result = L"success";
    } else {
      OnLogLine(L"Failed to rebuild session after refresh request.");
      std::scoped_lock lock(mutex_);
      stats_.last_device_change_result = L"rebuild-failed";
      stats_.last_rebuild_result = L"failed";
    }
  }
  RefreshCapabilitySnapshot();
}

bool AppModel::Start() {
  return controller_.Start(configuration_, this);
}

void AppModel::Stop() {
  controller_.Stop();
}

bool AppModel::Tick() {
  return controller_.Tick();
}

void AppModel::SetCaptureBackend(AudioBackendType backend) {
  {
    std::scoped_lock lock(mutex_);
    configuration_.capture.backend = backend;
  }
  RefreshDevices();
  RefreshCapabilitySnapshot();
}

void AppModel::SetRenderBackend(AudioBackendType backend) {
  {
    std::scoped_lock lock(mutex_);
    configuration_.render.backend = backend;
  }
  RefreshDevices();
  RefreshCapabilitySnapshot();
}

void AppModel::SetCaptureSourceMode(AudioSourceMode source_mode) {
  if ((source_mode == AudioSourceMode::ApplicationProcessLoopback ||
       source_mode == AudioSourceMode::ApplicationLoopback) &&
      !WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows()) {
    {
      std::scoped_lock lock(mutex_);
      configuration_.capture.source_mode = AudioSourceMode::MicrophoneCapture;
      configuration_.capture.application_loopback_target_kind =
          ApplicationLoopbackTargetKind::ApplicationName;
    }
    OnLogLine(WasapiCaptureAdapter::DescribeProcessLoopbackSupport());
    RefreshDevices();
    RefreshCapabilitySnapshot();
    return;
  }
  {
    std::scoped_lock lock(mutex_);
    if (session_state_ == L"Running") {
      return;
    }
    const auto previous_source_mode = configuration_.capture.source_mode;
    const bool leaving_system_loopback =
        previous_source_mode == AudioSourceMode::SystemLoopback &&
        source_mode != AudioSourceMode::SystemLoopback;
    const bool entering_system_loopback =
        source_mode == AudioSourceMode::SystemLoopback &&
        previous_source_mode != AudioSourceMode::SystemLoopback;
    if (leaving_system_loopback) {
      configuration_.render.monitor_enabled = non_loopback_monitor_preference_;
    }
    configuration_.capture.source_mode = source_mode;
    if (entering_system_loopback || source_mode == AudioSourceMode::SystemLoopback) {
      configuration_.render.monitor_enabled = false;
    }
    if (source_mode == AudioSourceMode::ApplicationProcessLoopback) {
      configuration_.capture.application_loopback_target_kind =
          ApplicationLoopbackTargetKind::ProcessId;
    } else if (source_mode == AudioSourceMode::ApplicationLoopback) {
      configuration_.capture.application_loopback_target_kind =
          ApplicationLoopbackTargetKind::ApplicationName;
    }
  }
  RefreshDevices();
  RefreshCapabilitySnapshot();
}

void AppModel::SetApplicationLoopbackTarget(
    ApplicationLoopbackTargetKind target_kind,
    const std::wstring& target_value) {
  {
    std::scoped_lock lock(mutex_);
    configuration_.capture.application_loopback_target_kind = target_kind;
    configuration_.capture.application_loopback_target_value = target_value;
  }
  RefreshCapabilitySnapshot();
}

void AppModel::SetCaptureDeviceId(const std::wstring& device_id) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.device_id = device_id;
}

void AppModel::SetRenderDeviceId(const std::wstring& device_id) {
  std::scoped_lock lock(mutex_);
  configuration_.render.device_id = device_id;
}

void AppModel::SetFixedDelayMs(uint32_t delay_ms) {
  std::scoped_lock lock(mutex_);
  configuration_.render.fixed_delay_ms = delay_ms;
}

void AppModel::SetDumpEnabled(bool enabled) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.dump_enabled = enabled;
}

void AppModel::SetDumpPath(const std::wstring& dump_path) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.dump_path = dump_path;
}

void AppModel::SetDumpFileType(DumpFileType file_type) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.dump_file_type = file_type;
}

void AppModel::SetCaptureSampleRate(uint32_t sample_rate) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.format.sample_rate = sample_rate;
  configuration_.capture.format.normalize();
}

void AppModel::SetCaptureChannels(uint16_t channels) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.format.channels = channels;
  configuration_.capture.format.normalize();
}

void AppModel::SetCaptureSampleType(AudioSampleType sample_type) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.format.sample_type = sample_type;
  configuration_.capture.format.normalize();
}

void AppModel::SetRenderSampleRate(uint32_t sample_rate) {
  std::scoped_lock lock(mutex_);
  configuration_.render.format.sample_rate = sample_rate;
  configuration_.render.format.normalize();
}

void AppModel::SetRenderChannels(uint16_t channels) {
  std::scoped_lock lock(mutex_);
  configuration_.render.format.channels = channels;
  configuration_.render.format.normalize();
}

void AppModel::SetRenderSampleType(AudioSampleType sample_type) {
  std::scoped_lock lock(mutex_);
  configuration_.render.format.sample_type = sample_type;
  configuration_.render.format.normalize();
}

void AppModel::SetCaptureWasapiShareMode(WasapiShareMode share_mode) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.wasapi_share_mode = share_mode;
}

void AppModel::SetCaptureWasapiDriveMode(WasapiDriveMode drive_mode) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.wasapi_drive_mode = drive_mode;
}

void AppModel::SetCaptureWasapiStreamCategory(WasapiStreamCategory category) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.wasapi_stream_category = category;
}

void AppModel::SetCaptureWasapiStreamOptions(WasapiStreamOptions options) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.wasapi_stream_options = options;
}

void AppModel::SetRenderWasapiShareMode(WasapiShareMode share_mode) {
  std::scoped_lock lock(mutex_);
  configuration_.render.wasapi_share_mode = share_mode;
}

void AppModel::SetRenderWasapiDriveMode(WasapiDriveMode drive_mode) {
  std::scoped_lock lock(mutex_);
  configuration_.render.wasapi_drive_mode = drive_mode;
}

void AppModel::SetRenderWasapiStreamCategory(WasapiStreamCategory category) {
  std::scoped_lock lock(mutex_);
  configuration_.render.wasapi_stream_category = category;
}

void AppModel::SetRenderWasapiStreamOptions(WasapiStreamOptions options) {
  std::scoped_lock lock(mutex_);
  configuration_.render.wasapi_stream_options = options;
}

void AppModel::SetCaptureBufferDurationMs(uint32_t duration_ms) {
  std::scoped_lock lock(mutex_);
  configuration_.capture.buffer_duration_ms = duration_ms;
}

void AppModel::SetRenderBufferDurationMs(uint32_t duration_ms) {
  std::scoped_lock lock(mutex_);
  configuration_.render.buffer_duration_ms = duration_ms;
}

void AppModel::SetMonitorEnabled(bool enabled) {
  {
    std::scoped_lock lock(mutex_);
    if (configuration_.capture.source_mode == AudioSourceMode::SystemLoopback) {
      non_loopback_monitor_preference_ = enabled;
      configuration_.render.monitor_enabled = false;
    } else {
      non_loopback_monitor_preference_ = enabled;
      configuration_.render.monitor_enabled = enabled;
    }
  }
  RefreshCapabilitySnapshot();
}

void AppModel::SetFollowDefaultDevices(bool enabled) {
  {
    std::scoped_lock lock(mutex_);
    configuration_.follow_default_devices = enabled;
  }
  RefreshCapabilitySnapshot();
}

void AppModel::SetAutoAlignRenderFormat(bool enabled) {
  {
    std::scoped_lock lock(mutex_);
    configuration_.auto_align_render_format = enabled;
  }
  RefreshCapabilitySnapshot();
}

void AppModel::RecordProbeResult(const std::wstring& stage,
                                 uint32_t ticks,
                                 bool capture_wave_seen,
                                 bool render_wave_seen,
                                 const std::wstring& dump_path,
                                 uint64_t dump_bytes,
                                 uint64_t render_updates,
                                 const std::wstring& result,
                                 const std::wstring& failure_stage,
                                 const std::wstring& failure_reason) {
  std::scoped_lock lock(mutex_);
  probe_batch_lines_.clear();
  probe_stage_ = stage;
  probe_ticks_ = ticks;
  probe_capture_wave_seen_ = capture_wave_seen;
  probe_render_wave_seen_ = render_wave_seen;
  probe_dump_path_ = dump_path;
  probe_dump_bytes_ = dump_bytes;
  probe_dump_status_ = DescribeDumpStatus(dump_bytes);
  probe_render_updates_ = render_updates;
  probe_requested_capture_mode_.clear();
  probe_requested_render_mode_.clear();
  probe_requested_capture_device_id_.clear();
  probe_requested_render_device_id_.clear();
  probe_capture_mode_.clear();
  probe_render_mode_.clear();
  probe_requested_capture_format_.clear();
  probe_requested_render_format_.clear();
  probe_negotiated_capture_format_.clear();
  probe_negotiated_render_format_.clear();
  probe_resampler_mode_.clear();
  probe_capture_runtime_details_.clear();
  probe_render_runtime_details_.clear();
  probe_result_ = result;
  probe_failure_stage_ = failure_stage;
  probe_failure_reason_ = failure_reason;
  probe_render_wave_note_.clear();
}

bool AppModel::RunQuickProbe() {
  const bool was_running = session_state() == L"Running";
  {
    std::scoped_lock lock(mutex_);
    ClearWaveformCachesLocked();
  }
  SessionConfiguration probe_config = configuration_;
  if (!probe_config.capture.dump_enabled) {
    probe_config.capture.dump_enabled = true;
    if (probe_config.capture.dump_path.empty()) {
      probe_config.capture.dump_path.clear();
    }
  }

  RecordProbeResult(L"starting", 0, false, false, L"", 0, 0, L"running");
  {
    std::scoped_lock lock(mutex_);
    probe_requested_capture_mode_ =
        probe_config.capture.backend == AudioBackendType::Wasapi
            ? std::wstring(L"WASAPI ") +
                  ToWideString(probe_config.capture.wasapi_share_mode) + L" / " +
                  ToWideString(probe_config.capture.wasapi_drive_mode)
            : std::wstring(L"WAVE API Callback");
    probe_requested_render_mode_ =
        probe_config.render.backend == AudioBackendType::Wasapi
            ? std::wstring(L"WASAPI ") +
                  ToWideString(probe_config.render.wasapi_share_mode) + L" / " +
                  ToWideString(probe_config.render.wasapi_drive_mode)
            : std::wstring(L"WAVE API Callback");
    probe_requested_capture_device_id_ =
        probe_config.capture.device_id.empty()
            ? std::wstring(L"default")
            : probe_config.capture.device_id;
    probe_requested_render_device_id_ =
        probe_config.render.device_id.empty()
            ? std::wstring(L"default")
            : probe_config.render.device_id;
    probe_requested_capture_format_ =
        DescribeAudioFormat(probe_config.capture.format);
    probe_requested_render_format_ =
        DescribeAudioFormat(probe_config.render.format);
  }
  if (!controller_.Start(probe_config, this)) {
    const auto stats = controller_.diagnostics().stats;
    RecordProbeResult(L"start", 0, false, false, L"", 0,
                      stats.render_wave_updates, L"failed",
                      stats.last_error_stage, stats.last_error_message);
    {
      std::scoped_lock lock(mutex_);
      probe_requested_capture_mode_ =
          probe_config.capture.backend == AudioBackendType::Wasapi
              ? std::wstring(L"WASAPI ") +
                    ToWideString(probe_config.capture.wasapi_share_mode) + L" / " +
                    ToWideString(probe_config.capture.wasapi_drive_mode)
              : std::wstring(L"WAVE API Callback");
      probe_requested_render_mode_ =
          probe_config.render.backend == AudioBackendType::Wasapi
              ? std::wstring(L"WASAPI ") +
                    ToWideString(probe_config.render.wasapi_share_mode) + L" / " +
                    ToWideString(probe_config.render.wasapi_drive_mode)
              : std::wstring(L"WAVE API Callback");
      probe_requested_capture_device_id_ =
          probe_config.capture.device_id.empty()
              ? std::wstring(L"default")
              : probe_config.capture.device_id;
      probe_requested_render_device_id_ =
          probe_config.render.device_id.empty()
              ? std::wstring(L"default")
              : probe_config.render.device_id;
      probe_requested_capture_format_ =
          DescribeAudioFormat(probe_config.capture.format);
      probe_requested_render_format_ =
          DescribeAudioFormat(probe_config.render.format);
    }
    if (was_running) {
      controller_.Start(configuration_, this);
    }
    return false;
  }

  bool ok = true;
  int ticks = 0;
  bool saw_capture_wave = false;
  bool saw_render_wave = false;
  const auto min_probe_ticks = std::max<int>(
      8, static_cast<int>(probe_config.render.fixed_delay_ms / 10) + 8);
  const auto max_probe_ticks = std::max<int>(min_probe_ticks, 200);
  for (int index = 0; index < max_probe_ticks; ++index) {
    if (!controller_.Tick()) {
      ok = false;
      break;
    }
    ++ticks;

    const auto capture_wave = capture_waveform();
    const auto render_wave = render_waveform();
    saw_capture_wave = saw_capture_wave || !capture_wave.empty();
    saw_render_wave = saw_render_wave || !render_wave.empty();
    const bool capture_ready = saw_capture_wave;
    const bool render_ready =
        !probe_config.render.monitor_enabled || saw_render_wave;
    if (ticks >= min_probe_ticks && capture_ready && render_ready) {
      break;
    }
    if (UsesRealBackendFactory()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  controller_.Stop();
  const auto stats = this->stats();
  const bool monitor_expected = probe_config.render.monitor_enabled;
  std::wstring dump_path;
  uint64_t dump_bytes = 0;
  if (!stats.dump_path.empty()) {
    std::filesystem::path dump_file_path(stats.dump_path);
    uintmax_t bytes = 0;
    std::error_code ec;
    if (std::filesystem::exists(dump_file_path, ec)) {
      bytes = std::filesystem::file_size(dump_file_path, ec);
    }
    dump_bytes = static_cast<uint64_t>(bytes);
    dump_path = stats.dump_path;
  }
  if (ok && monitor_expected && !saw_render_wave) {
    ok = false;
    RecordProbeResult(L"render",
                      static_cast<uint32_t>(ticks),
                      saw_capture_wave,
                      false,
                      dump_path,
                      dump_bytes,
                      stats.render_wave_updates,
                      L"failed",
                      L"render-wave",
                      L"Render waveform was not observed during probe");
  } else if (ok && probe_config.capture.dump_enabled &&
             DescribeDumpStatus(dump_bytes) != L"data") {
    ok = false;
    RecordProbeResult(L"dump",
                      static_cast<uint32_t>(ticks),
                      saw_capture_wave,
                      monitor_expected ? saw_render_wave : false,
                      dump_path,
                      dump_bytes,
                      stats.render_wave_updates,
                      L"failed",
                      L"dump-data",
                      L"Dump file did not contain captured PCM data");
  } else if (ok) {
    RecordProbeResult(L"start",
                      static_cast<uint32_t>(ticks),
                      saw_capture_wave,
                      monitor_expected ? saw_render_wave : false,
                      dump_path,
                      dump_bytes,
                      stats.render_wave_updates,
                      L"success");
  } else {
    const auto fail_stats = this->stats();
    RecordProbeResult(L"tick",
                      static_cast<uint32_t>(ticks),
                      saw_capture_wave,
                      monitor_expected ? saw_render_wave : false,
                      dump_path,
                      dump_bytes,
                      fail_stats.render_wave_updates,
                      L"failed-during-tick",
                      fail_stats.last_error_stage,
                      fail_stats.last_error_message);
  }
  {
    std::scoped_lock lock(mutex_);
    probe_requested_capture_mode_ =
        probe_config.capture.backend == AudioBackendType::Wasapi
            ? std::wstring(L"WASAPI ") +
                  ToWideString(probe_config.capture.wasapi_share_mode) + L" / " +
                  ToWideString(probe_config.capture.wasapi_drive_mode)
            : std::wstring(L"WAVE API Callback");
    probe_requested_render_mode_ =
        probe_config.render.backend == AudioBackendType::Wasapi
            ? std::wstring(L"WASAPI ") +
                  ToWideString(probe_config.render.wasapi_share_mode) + L" / " +
                  ToWideString(probe_config.render.wasapi_drive_mode)
            : std::wstring(L"WAVE API Callback");
    probe_requested_capture_device_id_ =
        probe_config.capture.device_id.empty()
            ? std::wstring(L"default")
            : probe_config.capture.device_id;
    probe_requested_render_device_id_ =
        probe_config.render.device_id.empty()
            ? std::wstring(L"default")
            : probe_config.render.device_id;
    probe_capture_mode_ = stats.actual_capture_backend_mode;
    probe_render_mode_ = stats.actual_render_backend_mode;
    probe_requested_capture_format_ = stats.requested_capture_format;
    probe_requested_render_format_ = stats.requested_render_format;
    probe_negotiated_capture_format_ = stats.negotiated_capture_format;
    probe_negotiated_render_format_ = stats.negotiated_render_format;
    probe_resampler_mode_ = stats.actual_resampler_mode;
    probe_capture_runtime_details_ = stats.capture_runtime_details;
    probe_render_runtime_details_ = stats.render_runtime_details;
  }
  if (monitor_expected && !saw_render_wave) {
    std::scoped_lock lock(mutex_);
    probe_render_wave_note_ =
        L"render-wave-missing: current delay/buffer path may need longer probe duration";
  } else if (!monitor_expected) {
    std::scoped_lock lock(mutex_);
    probe_render_wave_note_ = L"monitor-disabled";
  }
  if (was_running) {
    controller_.Start(configuration_, this);
  }
  return ok;
}

void AppModel::RecordProbeBatchResult(const std::vector<std::wstring>& lines) {
  std::scoped_lock lock(mutex_);
  probe_stage_.clear();
  probe_ticks_ = 0;
  probe_capture_wave_seen_ = false;
  probe_render_wave_seen_ = false;
  probe_dump_path_.clear();
  probe_dump_bytes_ = 0;
  probe_dump_status_.clear();
  probe_render_updates_ = 0;
  probe_requested_capture_mode_.clear();
  probe_requested_render_mode_.clear();
  probe_requested_capture_device_id_.clear();
  probe_requested_render_device_id_.clear();
  probe_capture_mode_.clear();
  probe_render_mode_.clear();
  probe_requested_capture_format_.clear();
  probe_requested_render_format_.clear();
  probe_negotiated_capture_format_.clear();
  probe_negotiated_render_format_.clear();
  probe_resampler_mode_.clear();
  probe_capture_runtime_details_.clear();
  probe_render_runtime_details_.clear();
  probe_result_ = L"matrix";
  probe_failure_stage_.clear();
  probe_failure_reason_.clear();
  probe_render_wave_note_.clear();
  probe_batch_lines_ = lines;
}

bool AppModel::RunProbeMatrix() {
  return RunProbeMatrixFiltered(
      {AudioSourceMode::MicrophoneCapture, AudioSourceMode::SystemLoopback},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {WasapiShareMode::Shared, WasapiShareMode::Exclusive},
      {false, true},
      {L"PCM16-48k-stereo", L"PCM24-44k-mono"},
      {L"0ms", L"120ms"},
      {L"cap40-ren40", L"cap80-ren120"});
}

bool AppModel::RunProbeMatrixForSources(
    const std::vector<AudioSourceMode>& source_modes) {
  return RunProbeMatrixFiltered(
      source_modes, {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {WasapiShareMode::Shared, WasapiShareMode::Exclusive}, {false, true},
      {L"PCM16-48k-stereo", L"PCM24-44k-mono"},
      {L"0ms", L"120ms"},
      {L"cap40-ren40", L"cap80-ren120"});
}

bool AppModel::RunProbeMatrixForSourcesAndRenderBackends(
    const std::vector<AudioSourceMode>& source_modes,
    const std::vector<AudioBackendType>& render_backends) {
  return RunProbeMatrixFiltered(
      source_modes, {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      render_backends, {WasapiShareMode::Shared, WasapiShareMode::Exclusive},
      {false, true},
      {L"PCM16-48k-stereo", L"PCM24-44k-mono"},
      {L"0ms", L"120ms"},
      {L"cap40-ren40", L"cap80-ren120"});
}

bool AppModel::RunProbeMatrixForSourcesAndBackends(
    const std::vector<AudioSourceMode>& source_modes,
    const std::vector<AudioBackendType>& capture_backends,
    const std::vector<AudioBackendType>& render_backends) {
  return RunProbeMatrixFiltered(source_modes, capture_backends, render_backends,
                                {WasapiShareMode::Shared, WasapiShareMode::Exclusive},
                                {false, true},
                                {L"PCM16-48k-stereo", L"PCM24-44k-mono"},
                                {L"0ms", L"120ms"},
                                {L"cap40-ren40", L"cap80-ren120"});
}

bool AppModel::RunProbeMatrixFiltered(
    const std::vector<AudioSourceMode>& source_modes,
    const std::vector<AudioBackendType>& capture_backends,
    const std::vector<AudioBackendType>& render_backends,
    const std::vector<WasapiShareMode>& wasapi_share_modes,
    const std::vector<bool>& align_modes_filter,
    const std::vector<std::wstring>& profile_labels,
    const std::vector<std::wstring>& delay_labels,
    const std::vector<std::wstring>& buffer_labels) {
  const bool was_running = session_state() == L"Running";
  std::vector<std::wstring> lines;
  const SessionConfiguration base = configuration_;

  struct ProbeProfile {
    const wchar_t* label;
    AudioSampleType sample_type;
    uint32_t sample_rate;
    uint16_t channels;
  };
  const std::vector<ProbeProfile> profiles = {
      {L"PCM16-48k-stereo", AudioSampleType::PcmInt16, 48000, 2},
      {L"PCM24-44k-mono", AudioSampleType::PcmInt24, 44100, 1},
  };
  struct BufferProfile {
    const wchar_t* label;
    uint32_t capture_buffer_ms;
    uint32_t render_buffer_ms;
  };
  const std::vector<BufferProfile> buffer_profiles = {
      {L"cap40-ren40", 40, 40},
      {L"cap80-ren120", 80, 120},
  };
  struct DelayProfile {
    const wchar_t* label;
    uint32_t delay_ms;
  };
  const std::vector<DelayProfile> delay_profiles = {
      {L"0ms", 0},
      {L"120ms", 120},
  };

  const std::vector<bool> align_modes = {false, true};
  const std::vector<AudioBackendType> backends = {AudioBackendType::Wasapi,
                                                  AudioBackendType::WaveApi};
  struct WasapiModeProfile {
    WasapiShareMode share_mode;
    WasapiDriveMode drive_mode;
  };
  const std::vector<WasapiModeProfile> wasapi_mode_profiles = {
      {WasapiShareMode::Shared, WasapiDriveMode::EventDriven},
      {WasapiShareMode::Exclusive, WasapiDriveMode::TimerDriven},
  };
  for (const auto source_mode : source_modes) {
    for (const auto auto_align : align_modes) {
      if (std::find(align_modes_filter.begin(), align_modes_filter.end(),
                    auto_align) == align_modes_filter.end()) {
        continue;
      }
      for (const auto& delay_profile : delay_profiles) {
        if (!delay_labels.empty() &&
            std::find(delay_labels.begin(), delay_labels.end(),
                      std::wstring(delay_profile.label)) == delay_labels.end()) {
          continue;
        }
        for (const auto& profile : profiles) {
          if (std::find(profile_labels.begin(), profile_labels.end(),
                        std::wstring(profile.label)) == profile_labels.end()) {
            continue;
          }
          for (const auto& buffer_profile : buffer_profiles) {
            if (!buffer_labels.empty() &&
                std::find(buffer_labels.begin(), buffer_labels.end(),
                          std::wstring(buffer_profile.label)) == buffer_labels.end()) {
              continue;
            }
            for (const auto capture_backend : backends) {
              if (std::find(capture_backends.begin(), capture_backends.end(),
                            capture_backend) == capture_backends.end()) {
                continue;
              }
              for (const auto render_backend : backends) {
                if (std::find(render_backends.begin(), render_backends.end(),
                              render_backend) == render_backends.end()) {
                  continue;
                }
                const auto capture_modes =
                    capture_backend == AudioBackendType::Wasapi
                        ? wasapi_mode_profiles
                        : std::vector<WasapiModeProfile>{{WasapiShareMode::Shared,
                                                          WasapiDriveMode::EventDriven}};
                const auto render_modes =
                    render_backend == AudioBackendType::Wasapi
                        ? wasapi_mode_profiles
                        : std::vector<WasapiModeProfile>{{WasapiShareMode::Shared,
                                                          WasapiDriveMode::EventDriven}};
                for (const auto& capture_mode : capture_modes) {
                  if (capture_backend == AudioBackendType::Wasapi &&
                      std::find(wasapi_share_modes.begin(), wasapi_share_modes.end(),
                                capture_mode.share_mode) == wasapi_share_modes.end()) {
                    continue;
                  }
                  for (const auto& render_mode : render_modes) {
                    if (render_backend == AudioBackendType::Wasapi &&
                        std::find(wasapi_share_modes.begin(), wasapi_share_modes.end(),
                                  render_mode.share_mode) == wasapi_share_modes.end()) {
                      continue;
                    }
                    {
                      std::scoped_lock lock(mutex_);
                      ClearWaveformCachesLocked();
                    }
                    SessionConfiguration probe = base;
                    probe.capture.source_mode = source_mode;
                    probe.capture.backend = capture_backend;
                    probe.render.backend = render_backend;
                    probe.capture.dump_enabled = true;
                    probe.capture.dump_path.clear();
                    probe.auto_align_render_format = auto_align;
                    probe.render.fixed_delay_ms = delay_profile.delay_ms;
                    probe.capture.buffer_duration_ms = buffer_profile.capture_buffer_ms;
                    probe.render.buffer_duration_ms = buffer_profile.render_buffer_ms;
                    probe.capture.format.sample_type = profile.sample_type;
                    probe.capture.format.sample_rate = profile.sample_rate;
                    probe.capture.format.channels = profile.channels;
                    probe.capture.format.normalize();
                    if (!auto_align) {
                      probe.render.format.sample_type = profile.sample_type;
                      probe.render.format.sample_rate = profile.sample_rate;
                      probe.render.format.channels = profile.channels;
                      probe.render.format.normalize();
                    }
                    if (capture_backend == AudioBackendType::Wasapi) {
                      probe.capture.wasapi_share_mode = capture_mode.share_mode;
                      probe.capture.wasapi_drive_mode = capture_mode.drive_mode;
                    }
                    if (render_backend == AudioBackendType::Wasapi) {
                      probe.render.wasapi_share_mode = render_mode.share_mode;
                      probe.render.wasapi_drive_mode = render_mode.drive_mode;
                    }

                    const std::wstring requested_capture_mode =
                        capture_backend == AudioBackendType::Wasapi
                            ? std::wstring(L"WASAPI ") +
                                  ToWideString(probe.capture.wasapi_share_mode) + L" / " +
                                  ToWideString(probe.capture.wasapi_drive_mode)
                            : std::wstring(L"WAVE API Callback");
                    const std::wstring requested_render_mode =
                        render_backend == AudioBackendType::Wasapi
                            ? std::wstring(L"WASAPI ") +
                                  ToWideString(probe.render.wasapi_share_mode) + L" / " +
                                  ToWideString(probe.render.wasapi_drive_mode)
                            : std::wstring(L"WAVE API Callback");
                    const std::wstring requested_capture_device_id =
                        probe.capture.device_id.empty()
                            ? std::wstring(L"default")
                            : probe.capture.device_id;
                    const std::wstring requested_render_device_id =
                        probe.render.device_id.empty()
                            ? std::wstring(L"default")
                            : probe.render.device_id;

                    std::wstring line = ToWideString(source_mode) + L" | align=" +
                                        (auto_align ? std::wstring(L"on")
                                                    : std::wstring(L"off")) +
                                        L" | delay=" + delay_profile.label +
                                        L" | buf=" + buffer_profile.label +
                                        L" | profile=" + profile.label + L" | " +
                                        ToWideString(capture_backend) + L" -> " +
                                        ToWideString(render_backend) + L": ";

                    const bool loopback_exclusive_unsupported =
                        source_mode == AudioSourceMode::SystemLoopback &&
                        ((capture_backend == AudioBackendType::Wasapi &&
                          probe.capture.wasapi_share_mode == WasapiShareMode::Exclusive) ||
                         (render_backend == AudioBackendType::Wasapi &&
                          probe.render.wasapi_share_mode == WasapiShareMode::Exclusive));
                    if (loopback_exclusive_unsupported) {
                      line += L"FAIL | cap-req=" + requested_capture_mode +
                              L" | ren-req=" + requested_render_mode +
                              L" | cap-dev=" + requested_capture_device_id +
                              L" | ren-dev=" + requested_render_device_id +
                              L" [unsupported-mode] {WASAPI loopback requires shared mode.}";
                      lines.push_back(line);
                      continue;
                    }

                    const bool started = controller_.Start(probe, this);
                    if (!started) {
                      const auto stats = controller_.diagnostics().stats;
                      line += L"FAIL | cap-req=" + requested_capture_mode +
                              L" | ren-req=" + requested_render_mode +
                              L" | cap-dev=" + requested_capture_device_id +
                              L" | ren-dev=" + requested_render_device_id;
                      if (!stats.last_error_stage.empty()) {
                        line += L" [" + stats.last_error_stage + L"]";
                      }
                      if (!stats.last_error_message.empty()) {
                        line += L" {" + stats.last_error_message + L"}";
                      }
                    } else {
                      bool ok = true;
                      int ticks = 0;
                      const auto min_probe_ticks = std::max<int>(
                          8, static_cast<int>(probe.render.fixed_delay_ms / 10) + 8);
                      const auto max_probe_ticks = std::max<int>(min_probe_ticks, 80);
                      bool saw_capture_wave = false;
                      bool saw_render_wave = false;
                      for (int index = 0; index < max_probe_ticks; ++index) {
                        if (!controller_.Tick()) {
                          ok = false;
                          break;
                        }
                        ++ticks;
                        const auto capture_wave = capture_waveform();
                        const auto render_wave = render_waveform();
                        saw_capture_wave = !capture_wave.empty();
                        saw_render_wave = !render_wave.empty();
                        const bool render_ready =
                            !probe.render.monitor_enabled || saw_render_wave;
                    if (ticks >= min_probe_ticks && saw_capture_wave &&
                        render_ready) {
                      break;
                    }
                    if (UsesRealBackendFactory()) {
                      std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                      }
                      const auto stats = controller_.diagnostics().stats;
                      const auto dump_path = stats.dump_path;
                      if (ok && !saw_capture_wave) {
                        line += L"CAPTURE_MISSING";
                      } else if (ok && probe.render.monitor_enabled &&
                                 !saw_render_wave) {
                        line += L"RENDER_MISSING";
                      } else {
                        line += ok ? L"PASS" : L"TICK_FAIL";
                      }
                      line += L" | ticks=" + std::to_wstring(ticks);
                      if (!stats.negotiated_capture_format.empty()) {
                        line += L" | cap=" + stats.negotiated_capture_format;
                      }
                      if (!stats.negotiated_render_format.empty()) {
                        line += L" | ren=" + stats.negotiated_render_format;
                      }
                      const bool capture_format_match =
                          !stats.requested_capture_format.empty() &&
                          stats.requested_capture_format ==
                              stats.negotiated_capture_format;
                      const bool render_format_match =
                          !stats.requested_render_format.empty() &&
                          stats.requested_render_format ==
                              stats.negotiated_render_format;
                      line += L" | cap-fmt-match=" +
                              std::wstring(capture_format_match ? L"matched"
                                                                : L"adjusted");
                      line += L" | ren-fmt-match=" +
                              std::wstring(render_format_match ? L"matched"
                                                               : L"adjusted");
                      line += L" | cap-req=" + requested_capture_mode;
                      line += L" | ren-req=" + requested_render_mode;
                      line += L" | cap-dev=" + requested_capture_device_id;
                      line += L" | ren-dev=" + requested_render_device_id;
                      bool mode_match = false;
                      if (!stats.actual_capture_backend_mode.empty()) {
                        line += L" | cap-mode=" + stats.actual_capture_backend_mode;
                      }
                      if (!stats.actual_render_backend_mode.empty()) {
                        line += L" | ren-mode=" + stats.actual_render_backend_mode;
                      }
                      mode_match =
                          !stats.actual_capture_backend_mode.empty() &&
                          !stats.actual_render_backend_mode.empty() &&
                          stats.actual_capture_backend_mode == requested_capture_mode &&
                          stats.actual_render_backend_mode == requested_render_mode;
                      line += L" | mode-match=" +
                              std::wstring(mode_match ? L"matched" : L"adjusted");
                      if (!stats.actual_resampler_mode.empty()) {
                        line += L" | resampler=" + stats.actual_resampler_mode;
                      }
                      line += L" | render-updates=" +
                              std::to_wstring(stats.render_wave_updates);
                      line += L" | capture-wave=" +
                              std::wstring(saw_capture_wave ? L"seen" : L"missing");
                      if (probe.render.monitor_enabled) {
                        line += L" | render-wave=" +
                                std::wstring(saw_render_wave ? L"seen"
                                                             : L"pending-or-missing");
                      } else {
                        line += L" | render-wave=disabled";
                      }
                      if (!ok && !stats.last_error_stage.empty()) {
                        line += L" [" + stats.last_error_stage + L"]";
                      }
                      controller_.Stop();
                      uint64_t dump_bytes = 0;
                      if (!dump_path.empty()) {
                        std::filesystem::path dump_file_path(dump_path);
                        std::error_code ec;
                        if (std::filesystem::exists(dump_file_path, ec)) {
                          dump_bytes = static_cast<uint64_t>(
                              std::filesystem::file_size(dump_file_path, ec));
                          std::filesystem::remove(dump_file_path, ec);
                        }
                      }
                      line += L" | dump-bytes=" + std::to_wstring(dump_bytes);
                      line += L" | dump-status=" + DescribeDumpStatus(dump_bytes);
                      lines.push_back(line);
                      continue;
                    }
                    controller_.Stop();
                    lines.push_back(line);
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  RecordProbeBatchResult(lines);
  if (was_running) {
    controller_.Start(configuration_, this);
  }
  return true;
}

void AppModel::HandleDefaultDeviceRefresh() {
  SessionConfiguration config_copy;
  bool should_restart = false;
  {
    std::scoped_lock lock(mutex_);
    should_restart = session_state_ == L"Running";
    const bool will_rebuild = should_restart && configuration_.follow_default_devices;
    stats_.last_device_change_reason = L"default-device-change";
    stats_.last_device_change_result =
        will_rebuild ? L"rebuilding-active-session" : L"tracked-no-rebuild";
    if (!will_rebuild) {
      stats_.last_rebuild_reason.clear();
      stats_.last_rebuild_result.clear();
    }
    if (configuration_.follow_default_devices) {
      configuration_.capture.device_id.clear();
      configuration_.render.device_id.clear();
    }
    config_copy = configuration_;
  }
  if (should_restart && config_copy.follow_default_devices) {
    controller_.Stop();
    OnLogLine(L"Default device change detected. Rebuilding active session.");
    {
      std::scoped_lock lock(mutex_);
      stats_.last_rebuild_reason = L"default-device-change";
      stats_.last_rebuild_result = L"restarting";
    }
  }
  controller_.RefreshDevices(config_copy);
  if (should_restart && config_copy.follow_default_devices) {
    if (controller_.Start(config_copy, this)) {
      std::scoped_lock lock(mutex_);
      stats_.last_device_change_result = L"rebuild-success";
      stats_.last_rebuild_reason = L"default-device-change";
      stats_.last_rebuild_result = L"success";
    } else {
      OnLogLine(L"Failed to rebuild session after default device change.");
      std::scoped_lock lock(mutex_);
      stats_.last_device_change_result = L"rebuild-failed";
      stats_.last_rebuild_reason = L"default-device-change";
      stats_.last_rebuild_result = L"failed";
    }
  }
}

SessionConfiguration AppModel::configuration() const {
  std::scoped_lock lock(mutex_);
  return configuration_;
}

DeviceEnumerationSnapshot AppModel::devices() const {
  std::scoped_lock lock(mutex_);
  return devices_;
}

SessionRuntimeStats AppModel::stats() const {
  std::scoped_lock lock(mutex_);
  return stats_;
}

std::vector<WaveformEnvelopePoint> AppModel::capture_waveform() const {
  std::scoped_lock lock(mutex_);
  return capture_waveform_;
}

std::vector<WaveformEnvelopePoint> AppModel::render_waveform() const {
  std::scoped_lock lock(mutex_);
  return render_waveform_;
}

std::vector<std::wstring> AppModel::logs() const {
  std::scoped_lock lock(mutex_);
  return logs_;
}

std::wstring AppModel::session_state() const {
  std::scoped_lock lock(mutex_);
  return session_state_;
}

std::wstring AppModel::summary_text() const {
  std::scoped_lock lock(mutex_);
  const auto effective_configured_render_request =
      configuration_.auto_align_render_format ? configuration_.capture.format
                                              : configuration_.render.format;
  std::wstring text = L"Capture: " + ToWideString(configuration_.capture.backend) +
                      L" / " + ToWideString(configuration_.capture.source_mode) +
                      L" / " + DescribeAudioFormat(configuration_.capture.format) +
                      L"\r\nRender: " + ToWideString(configuration_.render.backend) +
                      L" / " + DescribeAudioFormat(configuration_.render.format) +
                      L"\r\nCapture WASAPI: " +
                      ToWideString(configuration_.capture.wasapi_share_mode) + L" / " +
                      ToWideString(configuration_.capture.wasapi_drive_mode) +
                      L" / " +
                      ToWideString(configuration_.capture.wasapi_stream_category) +
                      L" / " +
                      ToWideString(configuration_.capture.wasapi_stream_options) +
                      L"\r\nRender WASAPI: " +
                      ToWideString(configuration_.render.wasapi_share_mode) + L" / " +
                      ToWideString(configuration_.render.wasapi_drive_mode) +
                      L" / " +
                      ToWideString(configuration_.render.wasapi_stream_category) +
                      L" / " +
                      ToWideString(configuration_.render.wasapi_stream_options) +
                      L"\r\nFollow defaults: " +
                      std::wstring(configuration_.follow_default_devices ? L"On"
                                                                        : L"Off") +
                      L"\r\nRender auto-align: " +
                      std::wstring(configuration_.auto_align_render_format ? L"On"
                                                                           : L"Off") +
                      L"\r\nMonitor: " +
                      std::wstring(configuration_.render.monitor_enabled ? L"On"
                                                                        : L"Off") +
                      L"\r\nMonitor delay: " +
                      std::to_wstring(configuration_.render.fixed_delay_ms) + L" ms" +
                      L"\r\nCapture buffer: " +
                      std::to_wstring(configuration_.capture.buffer_duration_ms) + L" ms" +
                      L"\r\nRender buffer: " +
                      std::to_wstring(configuration_.render.buffer_duration_ms) + L" ms" +
                      L"\r\nDump: " +
                      std::wstring(configuration_.capture.dump_enabled ? L"On" : L"Off") +
                      L" (" + ToWideString(configuration_.capture.dump_file_type) + L")";
  if (configuration_.auto_align_render_format) {
    text += L"\r\n" + BuildEffectiveRenderRequestSummaryText(
                          DescribeAudioFormat(effective_configured_render_request));
  }
  if (configuration_.capture.source_mode ==
          AudioSourceMode::ApplicationProcessLoopback ||
      configuration_.capture.source_mode == AudioSourceMode::ApplicationLoopback) {
    text += L"\r\n" + BuildApplicationLoopbackTargetSummaryText(
                          configuration_.capture.application_loopback_target_kind,
                          configuration_.capture.application_loopback_target_value);
  }
  if (!configuration_.capture.dump_path.empty()) {
    text += L"\r\nDump path: " + configuration_.capture.dump_path;
  } else if (configuration_.capture.dump_enabled) {
    text += L"\r\nDump path: Auto temp file";
  }
  const auto follow_defaults_note = BuildFollowDefaultsNoteText(
      configuration_.follow_default_devices,
      configuration_.capture.source_mode);
  if (!follow_defaults_note.empty()) {
    text += L"\r\n" + follow_defaults_note;
  }
  const auto running_session_note =
      BuildRunningSessionConfigurationNoteText(session_state_);
  if (!running_session_note.empty()) {
    text += L"\r\n" + running_session_note;
  }
  const auto running_device_change_note =
      BuildRunningDeviceChangeSummaryText(session_state_,
                                          stats_.last_device_change_reason,
                                          stats_.last_device_change_result);
  if (!running_device_change_note.empty()) {
    text += L"\r\n" + running_device_change_note;
  }
  const auto running_device_selection_drift_note =
      BuildRunningDeviceSelectionDriftSummaryText(
          session_state_, configuration_.follow_default_devices,
          configuration_.capture.device_id,
          configuration_.render.device_id, stats_.requested_capture_device_id,
          stats_.requested_render_device_id);
  if (!running_device_selection_drift_note.empty()) {
    text += L"\r\n" + running_device_selection_drift_note;
  }
  const auto monitor_disabled_note =
      BuildMonitorDisabledNoteText(configuration_.render.monitor_enabled,
                                   stats_.active_render_monitor_enabled,
                                   session_state_);
  if (!monitor_disabled_note.empty()) {
    text += L"\r\n" + monitor_disabled_note;
  }
  const auto source_mode = configuration_.capture.source_mode;
  const bool wasapi_capture_backend =
      configuration_.capture.backend == AudioBackendType::Wasapi;
  const auto loopback_capture_note = BuildLoopbackCaptureNoteText(source_mode);
  if (!loopback_capture_note.empty()) {
    text += L"\r\n" + loopback_capture_note;
  }
  const auto loopback_backend_note =
      BuildLoopbackBackendNoteText(source_mode, wasapi_capture_backend);
  if (!loopback_backend_note.empty()) {
    text += L"\r\n" + loopback_backend_note;
  }
  if (!WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows()) {
    text += L"\r\n" + WasapiCaptureAdapter::DescribeProcessLoopbackSupport();
  }
  if (source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      source_mode == AudioSourceMode::ApplicationLoopback) {
    const auto target_kind =
        configuration_.capture.application_loopback_target_kind;
    const auto target_value =
        configuration_.capture.application_loopback_target_value;
    if (target_value.empty()) {
      text += L"\r\n" + BuildApplicationLoopbackNoteText(target_kind, target_value);
    } else if (WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows()) {
      text += L"\r\n" + BuildApplicationLoopbackNoteText(target_kind, target_value);
    }
  }
  return text;
}

std::wstring AppModel::diagnostics_text() const {
  std::scoped_lock lock(mutex_);
  const auto effective_configured_render_request =
      configuration_.auto_align_render_format ? configuration_.capture.format
                                              : configuration_.render.format;
  const auto active_effective_render_request =
      stats_.effective_render_request_format.empty()
          ? DescribeAudioFormat(effective_configured_render_request)
          : stats_.effective_render_request_format;
  std::wstring text =
      BuildCurrentConfiguredCaptureDiagnosticsLabelText() +
      DescribeAudioFormat(configuration_.capture.format) +
      L"\r\n" +
      BuildCurrentConfiguredRenderDiagnosticsLabelText() +
      DescribeAudioFormat(configuration_.render.format) +
      L"\r\n" +
      BuildEffectiveConfiguredRenderRequestDiagnosticsLabelText() +
      active_effective_render_request +
      L"\r\n" +
      BuildActiveRequestedCaptureDiagnosticsLabelText() + stats_.requested_capture_format +
      L"\r\n" +
      BuildActiveRequestedRenderDiagnosticsLabelText() + stats_.requested_render_format +
      L"\r\n" +
      BuildActiveRequestedCaptureDeviceIdDiagnosticsLabelText() + stats_.requested_capture_device_id +
      L"\r\n" +
      BuildActiveRequestedRenderDeviceIdDiagnosticsLabelText() + stats_.requested_render_device_id +
      L"\r\n" +
      BuildActiveNegotiatedCaptureDiagnosticsLabelText() + stats_.negotiated_capture_format +
      L"\r\n" +
      BuildActiveNegotiatedRenderDiagnosticsLabelText() + stats_.negotiated_render_format +
      L"\r\n" +
      BuildActiveCaptureModeDiagnosticsLabelText() + stats_.actual_capture_backend_mode +
      L"\r\n" +
      BuildActiveRenderModeDiagnosticsLabelText() + stats_.actual_render_backend_mode +
      L"\r\n" +
      BuildActiveResamplerDiagnosticsLabelText() + stats_.actual_resampler_mode +
      L"\r\n" +
      BuildActiveCaptureRuntimeDiagnosticsLabelText() + stats_.capture_runtime_details +
      L"\r\n" +
      BuildActiveRenderRuntimeDiagnosticsLabelText() + stats_.render_runtime_details;
  if (stats_.active_requested_wasapi_mode_present) {
    text += L"\r\n" + BuildActiveCaptureWasapiRequestDiagnosticsLabelText() +
            stats_.requested_capture_wasapi_mode;
    text += L"\r\n" + BuildActiveRenderWasapiRequestDiagnosticsLabelText() +
            stats_.requested_render_wasapi_mode;
  }
  if (stats_.active_requested_timing_present) {
    text += L"\r\n" + BuildActiveMonitorDelayDiagnosticsLabelText() +
            std::to_wstring(stats_.requested_monitor_delay_ms) + L" ms";
    text += L"\r\n" + BuildActiveCaptureBufferDiagnosticsLabelText() +
            std::to_wstring(stats_.requested_capture_buffer_duration_ms) + L" ms";
    text += L"\r\n" + BuildActiveRenderBufferDiagnosticsLabelText() +
            std::to_wstring(stats_.requested_render_buffer_duration_ms) + L" ms";
  }
  const auto source_mode = configuration_.capture.source_mode;
  const bool suppress_last_rebuild =
      stats_.last_device_change_result == L"tracked-no-rebuild";
  if (!stats_.last_device_change_reason.empty()) {
    text += L"\r\n" + BuildLastDeviceChangeDiagnosticsText(
                           stats_.last_device_change_reason,
                           stats_.last_device_change_result);
  }
  if (!suppress_last_rebuild && !stats_.last_rebuild_reason.empty()) {
    text += L"\r\n" + BuildLastRebuildDiagnosticsText(
                           stats_.last_rebuild_reason,
                           stats_.last_rebuild_result);
  }
  if (!stats_.last_error_stage.empty()) {
    text += L"\r\nLast error: " + stats_.last_error_stage + L" => " +
            stats_.last_error_message;
  }
  const auto running_session_note =
      BuildRunningSessionConfigurationNoteText(session_state_);
  if (!running_session_note.empty()) {
    text += L"\r\n" + running_session_note;
  }
  const auto follow_defaults_diagnostics = BuildFollowDefaultsDiagnosticsText(
      configuration_.follow_default_devices, source_mode);
  if (!follow_defaults_diagnostics.empty()) {
    text += L"\r\n" + follow_defaults_diagnostics;
  }
  if (source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      source_mode == AudioSourceMode::ApplicationLoopback) {
    text += L"\r\n" + BuildApplicationLoopbackDiagnosticsText(
                        configuration_.capture.application_loopback_target_kind,
                        configuration_.capture.application_loopback_target_value);
  }
  const auto monitor_disabled_diagnostics =
      BuildMonitorDisabledDiagnosticsText(
          configuration_.render.monitor_enabled,
          stats_.active_render_monitor_enabled,
          session_state_);
  if (!monitor_disabled_diagnostics.empty()) {
    text += L"\r\n" + monitor_disabled_diagnostics;
  }
  for (const auto& device : devices_.capture_devices) {
    if (device.id == configuration_.capture.device_id) {
      text += L"\r\n" +
              BuildSelectedCaptureDeviceDiagnosticsLabelText(source_mode);
      text += DescribeDeviceCapabilities(device);
      text += L"\r\n" +
              BuildSelectedCaptureDeviceIdDiagnosticsLabelText(source_mode);
      text += device.id;
      break;
    }
  }
  for (const auto& device : devices_.render_devices) {
    if (device.id == configuration_.render.device_id) {
      text += L"\r\nSelected render device: " + DescribeDeviceCapabilities(device);
      text += L"\r\nSelected render id: " + device.id;
      break;
    }
  }
  if (!stats_.dump_path.empty()) {
    text += L"\r\nActive dump path: " + stats_.dump_path;
  }
  return text;
}

std::wstring AppModel::capability_text() const {
  std::scoped_lock lock(mutex_);
  return capability_text_cache_;
}

std::wstring AppModel::probe_text() const {
  std::scoped_lock lock(mutex_);
  std::wstring text =
      L"Probe\r\n"
      L"- Use Start Session for sustained validation\r\n"
      L"- Short probe status is stored here when available";
  if (!probe_batch_lines_.empty()) {
    int pass_count = 0;
    int fail_count = 0;
    int source_mode_fail_count = 0;
    int unsupported_mode_fail_count = 0;
    int capture_device_fail_count = 0;
    int render_device_fail_count = 0;
    int capture_start_fail_count = 0;
    int render_start_fail_count = 0;
    int format_resolution_fail_count = 0;
    int mode_matched_count = 0;
    int mode_adjusted_count = 0;
    int capture_format_matched_count = 0;
    int capture_format_adjusted_count = 0;
    int render_format_matched_count = 0;
    int render_format_adjusted_count = 0;
    int dump_data_count = 0;
    int dump_header_only_count = 0;
    int dump_none_count = 0;
    int capture_missing_count = 0;
    int render_missing_count = 0;
    int tick_fail_count = 0;
    std::map<std::wstring, MatrixBucketSummary> backend_summaries;
    std::map<std::wstring, MatrixBucketSummary> pair_summaries;
    std::map<std::wstring, MatrixBucketSummary> profile_summaries;
    std::map<std::wstring, MatrixBucketSummary> align_summaries;
    std::map<std::wstring, MatrixBucketSummary> source_summaries;
    std::map<std::wstring, MatrixBucketSummary> delay_summaries;
    std::map<std::wstring, MatrixBucketSummary> buffer_summaries;
    for (const auto& line : probe_batch_lines_) {
      const auto backend_pos = line.find(L"WASAPI ->");
      const auto backend_wave_pos = line.find(L"WAVE API ->");
      size_t backend_start = std::wstring::npos;
      if (backend_pos != std::wstring::npos) {
        backend_start = backend_pos;
      } else if (backend_wave_pos != std::wstring::npos) {
        backend_start = backend_wave_pos;
      }
      if (backend_start != std::wstring::npos) {
        const auto backend_end = line.find(L": ", backend_start);
        if (backend_end != std::wstring::npos) {
          const auto backend_key =
              line.substr(backend_start, backend_end - backend_start);
          auto& bucket = backend_summaries[backend_key];
          if (line.find(L": PASS") != std::wstring::npos) {
            ++bucket.pass;
          } else if (line.find(L": FAIL") != std::wstring::npos) {
            ++bucket.fail;
          } else if (line.find(L"CAPTURE_MISSING") != std::wstring::npos) {
            ++bucket.capture_missing;
          } else if (line.find(L"RENDER_MISSING") != std::wstring::npos) {
            ++bucket.render_missing;
          } else if (line.find(L"TICK_FAIL") != std::wstring::npos) {
            ++bucket.tick_fail;
          }
        }
      }
      const auto pair_pos = line.find(L" | profile=");
      if (pair_pos != std::wstring::npos) {
        const auto pair_end = line.find(L": ", pair_pos + 11);
        if (pair_end != std::wstring::npos) {
          const auto pair_start = pair_pos + 11;
          const auto pair_key = line.substr(pair_start, pair_end - pair_start);
          auto& bucket = pair_summaries[pair_key];
          if (line.find(L": PASS") != std::wstring::npos) {
            ++bucket.pass;
          } else if (line.find(L": FAIL") != std::wstring::npos) {
            ++bucket.fail;
          } else if (line.find(L"CAPTURE_MISSING") != std::wstring::npos) {
            ++bucket.capture_missing;
          } else if (line.find(L"RENDER_MISSING") != std::wstring::npos) {
            ++bucket.render_missing;
          } else if (line.find(L"TICK_FAIL") != std::wstring::npos) {
            ++bucket.tick_fail;
          }
        }
      }
      const auto profile_pos = line.find(L"profile=");
      if (profile_pos != std::wstring::npos) {
        const auto profile_end = line.find(L" | ", profile_pos);
        const auto profile_key =
            line.substr(profile_pos + 8,
                        (profile_end == std::wstring::npos ? line.size()
                                                           : profile_end) -
                            (profile_pos + 8));
        auto& bucket = profile_summaries[profile_key];
        if (line.find(L": PASS") != std::wstring::npos) {
          ++bucket.pass;
        } else if (line.find(L": FAIL") != std::wstring::npos) {
          ++bucket.fail;
        } else if (line.find(L"CAPTURE_MISSING") != std::wstring::npos) {
          ++bucket.capture_missing;
        } else if (line.find(L"RENDER_MISSING") != std::wstring::npos) {
          ++bucket.render_missing;
        } else if (line.find(L"TICK_FAIL") != std::wstring::npos) {
          ++bucket.tick_fail;
        }
      }
      const auto align_pos = line.find(L"align=");
      if (align_pos != std::wstring::npos) {
        const auto align_end = line.find(L" | ", align_pos);
        const auto align_key =
            line.substr(align_pos + 6,
                        (align_end == std::wstring::npos ? line.size()
                                                         : align_end) -
                            (align_pos + 6));
        auto& bucket = align_summaries[align_key];
        if (line.find(L": PASS") != std::wstring::npos) {
          ++bucket.pass;
        } else if (line.find(L": FAIL") != std::wstring::npos) {
          ++bucket.fail;
        } else if (line.find(L"CAPTURE_MISSING") != std::wstring::npos) {
          ++bucket.capture_missing;
        } else if (line.find(L"RENDER_MISSING") != std::wstring::npos) {
          ++bucket.render_missing;
        } else if (line.find(L"TICK_FAIL") != std::wstring::npos) {
          ++bucket.tick_fail;
        }
      }
      const auto source_sep = line.find(L" | ");
      if (source_sep != std::wstring::npos) {
        const auto source_key = line.substr(0, source_sep);
        auto& bucket = source_summaries[source_key];
        if (line.find(L": PASS") != std::wstring::npos) {
          ++bucket.pass;
        } else if (line.find(L": FAIL") != std::wstring::npos) {
          ++bucket.fail;
        } else if (line.find(L"CAPTURE_MISSING") != std::wstring::npos) {
          ++bucket.capture_missing;
        } else if (line.find(L"RENDER_MISSING") != std::wstring::npos) {
          ++bucket.render_missing;
        } else if (line.find(L"TICK_FAIL") != std::wstring::npos) {
          ++bucket.tick_fail;
        }
      }
      const auto delay_pos = line.find(L"delay=");
      if (delay_pos != std::wstring::npos) {
        const auto delay_end = line.find(L" | ", delay_pos);
        const auto delay_key =
            line.substr(delay_pos + 6,
                        (delay_end == std::wstring::npos ? line.size()
                                                         : delay_end) -
                            (delay_pos + 6));
        auto& bucket = delay_summaries[delay_key];
        if (line.find(L": PASS") != std::wstring::npos) {
          ++bucket.pass;
        } else if (line.find(L": FAIL") != std::wstring::npos) {
          ++bucket.fail;
        } else if (line.find(L"CAPTURE_MISSING") != std::wstring::npos) {
          ++bucket.capture_missing;
        } else if (line.find(L"RENDER_MISSING") != std::wstring::npos) {
          ++bucket.render_missing;
        } else if (line.find(L"TICK_FAIL") != std::wstring::npos) {
          ++bucket.tick_fail;
        }
      }
      const auto buf_pos = line.find(L"buf=");
      if (buf_pos != std::wstring::npos) {
        const auto buf_end = line.find(L" | ", buf_pos);
        const auto buf_key =
            line.substr(buf_pos + 4,
                        (buf_end == std::wstring::npos ? line.size() : buf_end) -
                            (buf_pos + 4));
        auto& bucket = buffer_summaries[buf_key];
        if (line.find(L": PASS") != std::wstring::npos) {
          ++bucket.pass;
        } else if (line.find(L": FAIL") != std::wstring::npos) {
          ++bucket.fail;
        } else if (line.find(L"CAPTURE_MISSING") != std::wstring::npos) {
          ++bucket.capture_missing;
        } else if (line.find(L"RENDER_MISSING") != std::wstring::npos) {
          ++bucket.render_missing;
        } else if (line.find(L"TICK_FAIL") != std::wstring::npos) {
          ++bucket.tick_fail;
        }
      }
      if (line.find(L"mode-match=matched") != std::wstring::npos) {
        ++mode_matched_count;
      } else if (line.find(L"mode-match=adjusted") != std::wstring::npos) {
          ++mode_adjusted_count;
        }
        if (line.find(L"cap-fmt-match=matched") != std::wstring::npos) {
          ++capture_format_matched_count;
        } else if (line.find(L"cap-fmt-match=adjusted") != std::wstring::npos) {
          ++capture_format_adjusted_count;
        }
        if (line.find(L"ren-fmt-match=matched") != std::wstring::npos) {
          ++render_format_matched_count;
        } else if (line.find(L"ren-fmt-match=adjusted") != std::wstring::npos) {
          ++render_format_adjusted_count;
        }
        if (line.find(L"dump-status=data") != std::wstring::npos) {
          ++dump_data_count;
        } else if (line.find(L"dump-status=header-only") != std::wstring::npos) {
          ++dump_header_only_count;
        } else if (line.find(L"dump-status=none") != std::wstring::npos) {
          ++dump_none_count;
        }
        if (line.find(L"CAPTURE_MISSING") != std::wstring::npos) {
          ++capture_missing_count;
        } else if (line.find(L"RENDER_MISSING") != std::wstring::npos) {
        ++render_missing_count;
      } else if (line.find(L"TICK_FAIL") != std::wstring::npos) {
        ++tick_fail_count;
      } else if (line.find(L": PASS") != std::wstring::npos) {
        ++pass_count;
      } else if (line.find(L": FAIL") != std::wstring::npos) {
        ++fail_count;
        if (line.find(L"[source-mode]") != std::wstring::npos) {
          ++source_mode_fail_count;
        }
        if (line.find(L"[unsupported-mode]") != std::wstring::npos) {
          ++unsupported_mode_fail_count;
        }
        if (line.find(L"[format-resolution]") != std::wstring::npos) {
          ++format_resolution_fail_count;
        }
        if (line.find(L"[capture-device]") != std::wstring::npos) {
          ++capture_device_fail_count;
        }
        if (line.find(L"[render-device]") != std::wstring::npos) {
          ++render_device_fail_count;
        }
        if (line.find(L"[capture-start]") != std::wstring::npos) {
          ++capture_start_fail_count;
        }
        if (line.find(L"[render-start]") != std::wstring::npos) {
          ++render_start_fail_count;
        }
      }
    }
    text += L"\r\n- Last probe matrix";
    text += L"\r\n- MatrixSummary: PASS=" + std::to_wstring(pass_count) +
            L" FAIL=" + std::to_wstring(fail_count) +
            L" SOURCE_MODE=" + std::to_wstring(source_mode_fail_count) +
            L" UNSUPPORTED_MODE=" + std::to_wstring(unsupported_mode_fail_count) +
            L" CAPTURE_DEVICE_FAIL=" + std::to_wstring(capture_device_fail_count) +
            L" RENDER_DEVICE_FAIL=" + std::to_wstring(render_device_fail_count) +
            L" CAPTURE_START_FAIL=" + std::to_wstring(capture_start_fail_count) +
            L" RENDER_START_FAIL=" + std::to_wstring(render_start_fail_count) +
            L" FORMAT_RESOLUTION=" +
            std::to_wstring(format_resolution_fail_count) +
            L" MODE_MATCHED=" + std::to_wstring(mode_matched_count) +
            L" MODE_ADJUSTED=" + std::to_wstring(mode_adjusted_count) +
            L" CAP_FMT_MATCHED=" +
            std::to_wstring(capture_format_matched_count) +
            L" CAP_FMT_ADJUSTED=" +
            std::to_wstring(capture_format_adjusted_count) +
            L" REN_FMT_MATCHED=" +
            std::to_wstring(render_format_matched_count) +
            L" REN_FMT_ADJUSTED=" +
            std::to_wstring(render_format_adjusted_count) +
            L" DUMP_DATA=" + std::to_wstring(dump_data_count) +
            L" DUMP_HEADER_ONLY=" +
            std::to_wstring(dump_header_only_count) +
            L" DUMP_NONE=" + std::to_wstring(dump_none_count) +
            L" CAPTURE_MISSING=" + std::to_wstring(capture_missing_count) +
            L" RENDER_MISSING=" + std::to_wstring(render_missing_count) +
            L" TICK_FAIL=" + std::to_wstring(tick_fail_count);
    if (capture_missing_count > 0 && source_mode_fail_count > 0 &&
        render_device_fail_count == 0 && render_start_fail_count == 0) {
      text +=
          L"\r\n- MatrixHint: capture missing is clustering with source-mode failures. "
          L"Check whether the selected capture backend/source combination can actually produce the requested loopback or microphone stream.";
    }
    const bool is_wasapi_wave_only =
        backend_summaries.size() == 1 &&
        backend_summaries.find(L"WASAPI -> WAVE API") !=
            backend_summaries.end();
    if (is_wasapi_wave_only && capture_missing_count > 0 &&
        unsupported_mode_fail_count > 0 && source_mode_fail_count == 0 &&
        render_device_fail_count == 0) {
      text +=
          L"\r\n- MatrixHint: this WASAPI-capture / WAVE-render loopback view is dominated by capture-missing and unsupported-mode results. Exclusive WASAPI capture modes are invalid here; compare against --matrix-render-backend=wasapi or the explicit-device loopback view to isolate WAVE render support on this machine.";
    }
    if (render_device_fail_count > 0 && capture_device_fail_count == 0 &&
        render_start_fail_count == 0) {
      text +=
          L"\r\n- MatrixHint: render-device failures are clustering. "
          L"Check whether the selected render device id matches the current render backend, or rerun without an explicit render-device-id override.";
    }
    for (const auto& [key, bucket] : backend_summaries) {
      text += L"\r\n- BackendSummary: " + key + L" PASS=" +
              std::to_wstring(bucket.pass) + L" FAIL=" +
              std::to_wstring(bucket.fail) + L" CAPTURE_MISSING=" +
              std::to_wstring(bucket.capture_missing) + L" RENDER_MISSING=" +
              std::to_wstring(bucket.render_missing) + L" TICK_FAIL=" +
              std::to_wstring(bucket.tick_fail);
    }
    for (const auto& [key, bucket] : pair_summaries) {
      text += L"\r\n- PairSummary: " + key + L" PASS=" +
              std::to_wstring(bucket.pass) + L" FAIL=" +
              std::to_wstring(bucket.fail) + L" CAPTURE_MISSING=" +
              std::to_wstring(bucket.capture_missing) + L" RENDER_MISSING=" +
              std::to_wstring(bucket.render_missing) + L" TICK_FAIL=" +
              std::to_wstring(bucket.tick_fail);
    }
    for (const auto& [key, bucket] : profile_summaries) {
      text += L"\r\n- ProfileSummary: " + key + L" PASS=" +
              std::to_wstring(bucket.pass) + L" FAIL=" +
              std::to_wstring(bucket.fail) + L" CAPTURE_MISSING=" +
              std::to_wstring(bucket.capture_missing) + L" RENDER_MISSING=" +
              std::to_wstring(bucket.render_missing) + L" TICK_FAIL=" +
              std::to_wstring(bucket.tick_fail);
    }
    for (const auto& [key, bucket] : align_summaries) {
      text += L"\r\n- AlignSummary: " + key + L" PASS=" +
              std::to_wstring(bucket.pass) + L" FAIL=" +
              std::to_wstring(bucket.fail) + L" CAPTURE_MISSING=" +
              std::to_wstring(bucket.capture_missing) + L" RENDER_MISSING=" +
              std::to_wstring(bucket.render_missing) + L" TICK_FAIL=" +
              std::to_wstring(bucket.tick_fail);
    }
    for (const auto& [key, bucket] : source_summaries) {
      text += L"\r\n- SourceSummary: " + key + L" PASS=" +
              std::to_wstring(bucket.pass) + L" FAIL=" +
              std::to_wstring(bucket.fail) + L" CAPTURE_MISSING=" +
              std::to_wstring(bucket.capture_missing) + L" RENDER_MISSING=" +
              std::to_wstring(bucket.render_missing) + L" TICK_FAIL=" +
              std::to_wstring(bucket.tick_fail);
    }
    for (const auto& [key, bucket] : delay_summaries) {
      text += L"\r\n- DelaySummary: " + key + L" PASS=" +
              std::to_wstring(bucket.pass) + L" FAIL=" +
              std::to_wstring(bucket.fail) + L" CAPTURE_MISSING=" +
              std::to_wstring(bucket.capture_missing) + L" RENDER_MISSING=" +
              std::to_wstring(bucket.render_missing) + L" TICK_FAIL=" +
              std::to_wstring(bucket.tick_fail);
    }
    for (const auto& [key, bucket] : buffer_summaries) {
      text += L"\r\n- BufferSummary: " + key + L" PASS=" +
              std::to_wstring(bucket.pass) + L" FAIL=" +
              std::to_wstring(bucket.fail) + L" CAPTURE_MISSING=" +
              std::to_wstring(bucket.capture_missing) + L" RENDER_MISSING=" +
              std::to_wstring(bucket.render_missing) + L" TICK_FAIL=" +
              std::to_wstring(bucket.tick_fail);
    }
    for (const auto& line : probe_batch_lines_) {
      text += L"\r\n  " + line;
    }
    return text;
  }
  if (!probe_result_.empty()) {
    const auto dump_file = probe_dump_path_.empty()
                               ? std::wstring(L"none")
                               : std::filesystem::path(probe_dump_path_).filename().wstring();
    const bool render_disabled = probe_render_wave_note_ == L"monitor-disabled";
    const bool negotiation_reached =
        !probe_negotiated_capture_format_.empty() ||
        !probe_negotiated_render_format_.empty();
    const bool runtime_reached =
        !probe_capture_mode_.empty() ||
        !probe_render_mode_.empty() ||
        !probe_resampler_mode_.empty() ||
        !probe_capture_runtime_details_.empty() ||
        !probe_render_runtime_details_.empty();
    const bool probe_stream_started =
        runtime_reached || probe_ticks_ > 0 || probe_capture_wave_seen_ ||
        probe_render_wave_seen_;
    const auto capture_wave_text =
        probe_capture_wave_seen_
            ? std::wstring(L"seen")
            : std::wstring(probe_stream_started ? L"missing" : L"not-started");
    const auto render_wave_text =
        render_disabled ? std::wstring(L"disabled")
                        : (probe_render_wave_seen_
                               ? std::wstring(L"seen")
                               : std::wstring(probe_stream_started ? L"missing"
                                                                   : L"not-started"));
    const auto waveform_text =
        (probe_capture_wave_seen_ || probe_render_wave_seen_)
            ? std::wstring(L"seen")
            : (probe_stream_started ? std::wstring(L"missing")
                               : std::wstring(L"not-started"));
    const auto negotiated_capture_text =
        probe_negotiated_capture_format_.empty()
            ? std::wstring(negotiation_reached ? L"unknown" : L"not-negotiated")
            : probe_negotiated_capture_format_;
    const auto negotiated_render_text =
        render_disabled
            ? std::wstring(L"disabled")
            : (probe_negotiated_render_format_.empty()
                   ? std::wstring(negotiation_reached ? L"unknown"
                                                      : L"not-negotiated")
                   : probe_negotiated_render_format_);
    const auto capture_mode_text =
        probe_capture_mode_.empty()
            ? std::wstring(runtime_reached ? L"unknown" : L"not-started")
            : probe_capture_mode_;
    const auto render_mode_text =
        render_disabled
            ? std::wstring(L"disabled")
            : (probe_render_mode_.empty()
                   ? std::wstring(runtime_reached ? L"unknown"
                                                  : L"not-started")
                   : probe_render_mode_);
    const auto resampler_text =
        probe_resampler_mode_.empty()
            ? std::wstring(runtime_reached ? L"unknown" : L"not-started")
            : probe_resampler_mode_;
    const auto capture_runtime_text =
        probe_capture_runtime_details_.empty()
            ? std::wstring(runtime_reached ? L"unknown" : L"not-started")
            : probe_capture_runtime_details_;
    const auto render_runtime_text =
        render_disabled
            ? std::wstring(L"disabled")
            : (probe_render_runtime_details_.empty()
                   ? std::wstring(runtime_reached ? L"unknown"
                                                  : L"not-started")
                   : probe_render_runtime_details_);
    const bool capture_format_match =
        !probe_requested_capture_format_.empty() &&
        probe_requested_capture_format_ == probe_negotiated_capture_format_;
    const bool render_format_match =
        !probe_requested_render_format_.empty() &&
        probe_requested_render_format_ == probe_negotiated_render_format_;
    const bool mode_match =
        !probe_requested_capture_mode_.empty() &&
        !probe_requested_render_mode_.empty() &&
        probe_requested_capture_mode_ == probe_capture_mode_ &&
        probe_requested_render_mode_ == probe_render_mode_;
    const auto capture_format_status =
        negotiation_reached ? std::wstring(capture_format_match ? L"matched"
                                                                : L"adjusted")
                            : std::wstring(L"not-negotiated");
    const auto render_format_status =
        render_disabled
            ? std::wstring(L"disabled")
            : (negotiation_reached
                   ? std::wstring(render_format_match ? L"matched"
                                                      : L"adjusted")
                   : std::wstring(L"not-negotiated"));
    const auto mode_status =
        render_disabled
            ? std::wstring(L"render-disabled")
            : (runtime_reached ? std::wstring(mode_match ? L"matched"
                                                         : L"adjusted")
                               : std::wstring(L"not-started"));
    text += L"\r\n- Last probe result: stage=" + probe_stage_ +
            L"; ticks=" + std::to_wstring(probe_ticks_) +
            L"; capture-wave=" + capture_wave_text +
            L"; render-wave=" + render_wave_text +
            L"; dump=" + (probe_dump_path_.empty() ? std::wstring(L"none") : probe_dump_path_) +
            L"; dump-file=" + dump_file +
            L"; dump-bytes=" + std::to_wstring(probe_dump_bytes_) +
            L"; result=" + probe_result_;
    text += L"\r\nQuickSummary: " + probe_result_ +
            L" | dump=" + (probe_dump_status_.empty() ? std::wstring(L"none")
                                                     : probe_dump_status_) +
            L" | cap-fmt=" + capture_format_status +
            L" | ren-fmt=" + render_format_status +
            L" | mode=" + mode_status +
            L" | monitor=" +
            std::wstring(render_disabled ? L"off" : L"on") +
            L" | cap-wave=" + capture_wave_text +
            L" | ren-wave=" + render_wave_text +
            L" | ren-updates=" + std::to_wstring(probe_render_updates_);

    text += L"\r\nStage: " + probe_stage_;
    text += L"\r\nTicks: " + std::to_wstring(probe_ticks_);
    text += L"\r\nWaveform: " + waveform_text;
    text += L"\r\nCaptureWave: " + capture_wave_text;
    text += L"\r\nRenderWave: " + render_wave_text;
    text += L"\r\nRequestedCapture: " +
            (probe_requested_capture_format_.empty() ? std::wstring(L"unknown")
                                                     : probe_requested_capture_format_);
    text += L"\r\nRequestedRender: " +
            (probe_requested_render_format_.empty() ? std::wstring(L"unknown")
                                                    : probe_requested_render_format_);
    text += L"\r\nNegotiatedCapture: " + negotiated_capture_text;
    text += L"\r\nNegotiatedRender: " + negotiated_render_text;
    text += L"\r\nCaptureFormatMatch: " + capture_format_status;
    text += L"\r\nRenderFormatMatch: " + render_format_status;
    if (!probe_render_wave_note_.empty()) {
      const auto render_wave_note_text =
          render_disabled
              ? BuildMonitorDisabledRenderWaveNoteText(false)
              : probe_render_wave_note_;
      text += L"\r\nRenderWaveNote: " + render_wave_note_text;
    }
    text += L"\r\nDump: " +
            (probe_dump_path_.empty() ? std::wstring(L"none") : probe_dump_path_);
    text += L"\r\nDumpFile: " + dump_file;
    text += L"\r\nDumpBytes: " + std::to_wstring(probe_dump_bytes_);
    text += L"\r\nDumpStatus: " +
            (probe_dump_status_.empty() ? std::wstring(L"none")
                                        : probe_dump_status_);
    text += L"\r\nRequestedCaptureMode: " +
            (probe_requested_capture_mode_.empty()
                 ? std::wstring(L"unknown")
                 : probe_requested_capture_mode_);
    text += L"\r\nRequestedRenderMode: " +
            (probe_requested_render_mode_.empty()
                 ? std::wstring(L"unknown")
                 : probe_requested_render_mode_);
    text += L"\r\nRequestedCaptureDeviceId: " +
            (probe_requested_capture_device_id_.empty()
                 ? std::wstring(L"unknown")
                 : probe_requested_capture_device_id_);
    text += L"\r\nRequestedRenderDeviceId: " +
            (probe_requested_render_device_id_.empty()
                 ? std::wstring(L"unknown")
                 : probe_requested_render_device_id_);
    text += L"\r\nCaptureMode: " + capture_mode_text;
    text += L"\r\nRenderMode: " + render_mode_text;
    text += L"\r\nResampler: " + resampler_text;
    text += L"\r\nCaptureRuntime: " + capture_runtime_text;
    text += L"\r\nRenderRuntime: " + render_runtime_text;
    text += L"\r\nRenderUpdates: " + std::to_wstring(probe_render_updates_);
    text += L"\r\nFailureStage: " +
            (probe_failure_stage_.empty() ? std::wstring(L"none")
                                          : probe_failure_stage_);
    text += L"\r\nFailureReason: " +
            (probe_failure_reason_.empty() ? std::wstring(L"none")
                                           : probe_failure_reason_);
    text += L"\r\nResult: " + probe_result_;
  }
  return text;
}

void AppModel::ClearWaveformCachesLocked() {
  capture_waveform_.clear();
  render_waveform_.clear();
}

void AppModel::OnLogLine(const std::wstring& line) {
  std::scoped_lock lock(mutex_);
  logs_.push_back(line);
  if (logs_.size() > 256) {
    logs_.erase(logs_.begin());
  }
}

void AppModel::OnStatsUpdated(const SessionRuntimeStats& stats) {
  std::scoped_lock lock(mutex_);
  auto merged = stats;
  if (merged.last_device_change_reason.empty() &&
      !stats_.last_device_change_reason.empty()) {
    merged.last_device_change_reason = stats_.last_device_change_reason;
    merged.last_device_change_result = stats_.last_device_change_result;
  }
  if (merged.last_rebuild_reason.empty() && !stats_.last_rebuild_reason.empty()) {
    merged.last_rebuild_reason = stats_.last_rebuild_reason;
    merged.last_rebuild_result = stats_.last_rebuild_result;
  }
  stats_ = std::move(merged);
}

void AppModel::OnWaveformUpdated(AudioDirection direction,
                                 const std::vector<WaveformEnvelopePoint>& waveform,
                                 const MeterValues& meter) {
  std::scoped_lock lock(mutex_);
  (void)meter;
  if (direction == AudioDirection::Capture) {
    capture_waveform_ = waveform;
  } else {
    render_waveform_ = waveform;
  }
}

void AppModel::OnDevicesUpdated(const DeviceEnumerationSnapshot& snapshot) {
  std::scoped_lock lock(mutex_);
  devices_ = snapshot;
  const auto capture_present = std::any_of(
      devices_.capture_devices.begin(), devices_.capture_devices.end(),
      [&](const AudioDeviceDescriptor& device) {
        return device.id == configuration_.capture.device_id;
      });
  if (configuration_.capture.device_id.empty() || !capture_present) {
    for (const auto& device : devices_.capture_devices) {
      if (device.is_default) {
        configuration_.capture.device_id = device.id;
        break;
      }
    }
  }
  const auto render_present = std::any_of(
      devices_.render_devices.begin(), devices_.render_devices.end(),
      [&](const AudioDeviceDescriptor& device) {
        return device.id == configuration_.render.device_id;
      });
  if (configuration_.render.device_id.empty() || !render_present) {
    for (const auto& device : devices_.render_devices) {
      if (device.is_default) {
        configuration_.render.device_id = device.id;
        break;
      }
    }
  }
}

void AppModel::RefreshCapabilitySnapshot() {
  std::scoped_lock lock(mutex_);
  std::wstring text =
      L"Current Capabilities\r\n"
      L"- WASAPI devices expose Shared/Exclusive/Event/Timer capability flags\r\n"
      L"- WAVE devices expose Callback-buffer capability\r\n"
      L"- Requested and negotiated formats are tracked independently\r\n"
      L"- Capture and render formats are independently configurable";

  if (!devices_.capture_devices.empty()) {
    text += L"\r\n- Capture devices visible: " +
            std::to_wstring(devices_.capture_devices.size());
  }
  if (!devices_.render_devices.empty()) {
    text += L"\r\n- Render devices visible: " +
            std::to_wstring(devices_.render_devices.size());
  }
  if (!WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows()) {
    text += L"\r\n- " + WasapiCaptureAdapter::DescribeProcessLoopbackSupport();
  }

  text += L"\r\n\r\nCurrent Limitations";
  if (configuration_.capture.source_mode ==
          AudioSourceMode::ApplicationProcessLoopback ||
      configuration_.capture.source_mode == AudioSourceMode::ApplicationLoopback) {
    text += WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows()
                ? L"\r\n- Application loopback requires a target process selection and process eligibility on this Windows build"
                : L"\r\n- " + WasapiCaptureAdapter::DescribeProcessLoopbackSupport();
  } else if (configuration_.capture.source_mode == AudioSourceMode::SystemLoopback &&
      configuration_.capture.backend == AudioBackendType::Wasapi) {
    text += L"\r\n- WASAPI loopback is shared-mode only";
    text += L"\r\n- System loopback disables monitor playback to avoid loopback storm";
  } else if (configuration_.capture.source_mode == AudioSourceMode::SystemLoopback &&
             configuration_.capture.backend == AudioBackendType::WaveApi) {
    text +=
        L"\r\n- WAVE loopback depends on hardware/driver-provided loopback devices";
  } else {
    text += L"\r\n- Device/driver format support still varies by actual hardware";
  }

  text += L"\r\n\r\nCurrent Strategy";
  if (configuration_.auto_align_render_format) {
    text +=
        L"\r\n- Render auto-align makes the effective render request follow the capture format";
  } else {
    text += L"\r\n- Render request uses its independently configured format";
  }

  text += L"\r\n\r\nSupport Probes";
  struct ProbeProfile {
    const wchar_t* label;
    AudioSampleType sample_type;
    uint32_t sample_rate;
    uint16_t channels;
  };
  const std::vector<ProbeProfile> profiles = {
      {L"PCM16-48k-stereo", AudioSampleType::PcmInt16, 48000, 2},
      {L"PCM24-44k-mono", AudioSampleType::PcmInt24, 44100, 1},
  };

  RealAudioBackendFactory factory;
  auto capture_adapter = factory.CreateCaptureAdapter(configuration_.capture.backend);
  auto render_adapter = factory.CreateRenderAdapter(configuration_.render.backend);
  for (const auto& profile : profiles) {
    CaptureConfig capture = configuration_.capture;
    capture.format.sample_type = profile.sample_type;
    capture.format.sample_rate = profile.sample_rate;
    capture.format.channels = profile.channels;
    capture.format.normalize();

    RenderConfig render = configuration_.render;
    render.format.sample_type = profile.sample_type;
    render.format.sample_rate = profile.sample_rate;
    render.format.channels = profile.channels;
    render.format.normalize();

    text += L"\r\n- Capture probe " + std::wstring(profile.label) + L": ";
    if (capture_adapter && capture_adapter->SupportsSource(capture.source_mode)) {
      const auto resolved = capture_adapter->GetPreferredFormat(capture);
      text += resolved.has_value() ? DescribeAudioFormat(*resolved) : L"unavailable";
    } else {
      text += L"unsupported";
    }

    text += L"\r\n- Render probe " + std::wstring(profile.label) + L": ";
    if (render_adapter) {
      const auto resolved = render_adapter->GetPreferredFormat(render);
      text += resolved.has_value() ? DescribeAudioFormat(*resolved) : L"unavailable";
    } else {
      text += L"unsupported";
    }
  }

  capability_text_cache_ = text;
}

void AppModel::OnSessionStateChanged(const std::wstring& state) {
  std::scoped_lock lock(mutex_);
  session_state_ = (state == L"Stopped") ? std::wstring(L"Idle") : state;
  if (state == L"Stopped") {
    ClearWaveformCachesLocked();
  }
}

}  // namespace winaudio
