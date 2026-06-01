#include "audio_session_controller.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace winaudio {

namespace {

bool ContainsDeviceId(const std::vector<AudioDeviceDescriptor>& devices,
                      const std::wstring& device_id) {
  return std::any_of(devices.begin(), devices.end(),
                     [&](const AudioDeviceDescriptor& device) {
                       return device.id == device_id;
                     });
}

std::wstring HumanizeAppLoopbackFailure(const std::wstring& detail) {
  if (detail == L"app-loopback-target-required") {
    return L"Application loopback requires a target process name or PID.";
  }
  if (detail == L"app-loopback-invalid-target") {
    return L"Application loopback target process was not found. Provide a running process name or PID.";
  }
  if (detail == L"app-loopback-unsupported-os") {
    return L"Application loopback is not supported on this machine. Windows process loopback capture requires client build 20348 or newer.";
  }
  if (detail.find(L"ActivateAudioInterfaceAsync: 0x8000000E") !=
      std::wstring::npos) {
    return L"Application loopback activation failed on this machine. The target process may not be eligible for loopback capture on this Windows build.";
  }
  if (detail.find(L"app-loopback-activate:") != std::wstring::npos) {
    return L"Application loopback activation did not complete successfully on this machine.";
  }
  return {};
}

}  // namespace

AudioSessionController::AudioSessionController(
    std::unique_ptr<IAudioBackendFactory> backend_factory)
    : backend_factory_(std::move(backend_factory)) {}

bool AudioSessionController::Initialize() {
  diagnostics_ = {};
  return backend_factory_ != nullptr;
}

DeviceEnumerationSnapshot AudioSessionController::RefreshDevices(
    const SessionConfiguration& config) {
  DeviceEnumerationSnapshot snapshot;

  auto capture_adapter = backend_factory_->CreateCaptureAdapter(config.capture.backend);
  auto render_adapter = backend_factory_->CreateRenderAdapter(config.render.backend);
  if (capture_adapter) {
    snapshot.capture_devices =
        capture_adapter->EnumerateDevices(config.capture.source_mode);
  }
  if (render_adapter) {
    snapshot.render_devices = render_adapter->EnumerateDevices();
  }

  if (sink_ != nullptr) {
    sink_->OnDevicesUpdated(snapshot);
  }
  return snapshot;
}

bool AudioSessionController::Start(const SessionConfiguration& config,
                                   ISessionEventSink* sink) {
  Stop();
  diagnostics_ = {};
  config_ = config;
  sink_ = sink;

  capture_adapter_ = backend_factory_->CreateCaptureAdapter(config.capture.backend);
  render_adapter_ = backend_factory_->CreateRenderAdapter(config.render.backend);
  resampler_ = CreateAudioResampler();
  dump_writer_ = std::make_unique<WavDumpWriter>();

  if (!capture_adapter_ || !render_adapter_ || !resampler_) {
    SetLastError(L"create-components", L"Failed to create one or more audio components.");
    Log(L"Failed to create one or more audio components.");
    return false;
  }

  if (!capture_adapter_->SupportsSource(config.capture.source_mode)) {
    std::wstring detail;
    if (config.capture.source_mode == AudioSourceMode::SystemLoopback) {
      detail =
          L"Selected backend does not support the chosen capture source. Use --capture-backend=wasapi for loopback, or switch --source=mic.";
    } else if (config.capture.source_mode ==
               AudioSourceMode::ApplicationLoopback) {
      detail =
          L"Selected backend does not support application loopback. Use --capture-backend=wasapi and provide --app-loopback-process on a supported Windows build.";
    } else {
      detail = L"Selected backend does not support the chosen capture source.";
    }
    diagnostics_.issues.push_back(
        {L"Source mode unsupported", detail});
    SetLastError(L"source-mode", detail);
    Log(L"Capture backend does not support the selected source mode.");
    return false;
  }

  if (config.capture.source_mode == AudioSourceMode::ApplicationLoopback &&
      config.capture.application_loopback_process.empty()) {
    const auto detail =
        std::wstring(
            L"Application loopback requires a target process name or PID. Provide --app-loopback-process before starting capture.");
    diagnostics_.issues.push_back({L"Application loopback target missing", detail});
    SetLastError(L"source-mode", detail);
    Log(L"Application loopback target process is missing.");
    return false;
  }

  if (!config.capture.device_id.empty()) {
    const auto capture_devices =
        capture_adapter_->EnumerateDevices(config.capture.source_mode);
    if (!ContainsDeviceId(capture_devices, config.capture.device_id)) {
      std::wstring detail;
      if (config.capture.source_mode == AudioSourceMode::SystemLoopback) {
        detail =
            L"Selected loopback capture device is not available for this source. Use devices --source=loopback to choose a render-backed loopback endpoint.";
      } else if (config.capture.source_mode ==
                 AudioSourceMode::ApplicationLoopback) {
        detail =
            L"Selected application loopback source is not available for this configuration. Use the built-in app-loopback source entry and provide --app-loopback-process.";
      } else {
        detail =
            L"Selected capture device is not available for this backend/source mode. Choose a device from devices for the same capture backend/source, or omit --capture-device-id.";
      }
      diagnostics_.issues.push_back({L"Capture device unavailable", detail});
      SetLastError((config.capture.source_mode == AudioSourceMode::SystemLoopback ||
                    config.capture.source_mode == AudioSourceMode::ApplicationLoopback)
                       ? L"source-mode"
                       : L"capture-device",
                   detail);
      Log(L"Selected capture device is not available for the requested source.");
      return false;
    }
  }

  if (config.render.monitor_enabled && !config.render.device_id.empty()) {
    const auto render_devices = render_adapter_->EnumerateDevices();
    if (!ContainsDeviceId(render_devices, config.render.device_id)) {
      const auto detail =
          std::wstring(
              L"Selected render device is not available for this backend. Choose a device from devices for the same render backend, or omit --render-device-id.");
      diagnostics_.issues.push_back({L"Render device unavailable", detail});
      SetLastError(L"render-device", detail);
      Log(L"Selected render device is not available for the requested backend.");
      return false;
    }
  }

  RenderConfig effective_render_config = config.render;
  if (config.auto_align_render_format) {
    effective_render_config.format = config.capture.format;
  }

  const auto capture_format = capture_adapter_->GetPreferredFormat(config.capture);
  const auto render_format =
      config.render.monitor_enabled
          ? render_adapter_->GetPreferredFormat(effective_render_config)
          : std::optional<AudioFormatSpec> {effective_render_config.format};
  if (!capture_format.has_value() || !render_format.has_value()) {
    std::wstring detail = L"Failed to resolve runtime audio formats.";
    if (!capture_format.has_value()) {
      const auto capture_detail = capture_adapter_->last_error();
      if (!capture_detail.empty()) {
        const auto app_loopback_reason = HumanizeAppLoopbackFailure(capture_detail);
        if (!app_loopback_reason.empty()) {
          detail += L" " + app_loopback_reason;
        } else {
          detail += L" capture=" + capture_detail;
        }
        if (capture_detail == L"wave-loopback-device-not-found") {
          detail += L" (source-mode unsupported on this machine)";
        } else if (capture_detail == L"app-loopback-target-required") {
          detail += L" (application loopback target process missing)";
        }
      }
    }
    if (config.render.monitor_enabled && !render_format.has_value()) {
      const auto render_detail = render_adapter_->last_error();
      if (!render_detail.empty()) {
        detail += L" render=" + render_detail;
      }
    }
    SetLastError(L"format-resolution", detail);
    Log(L"Failed to resolve runtime audio formats.");
    return false;
  }

  runtime_capture_format_ = *capture_format;
  runtime_render_format_ = *render_format;
  runtime_capture_format_.normalize();
  runtime_render_format_.normalize();

  if (!resampler_->Configure(runtime_capture_format_, runtime_render_format_)) {
    SetLastError(L"resampler-config", L"Failed to configure resampler.");
    Log(L"Failed to configure resampler.");
    return false;
  }

  const auto queue_frames = std::max<uint32_t>(
      runtime_capture_format_.sample_rate,
      runtime_capture_format_.sample_rate * config.render.fixed_delay_ms / 1000 +
          runtime_capture_format_.sample_rate / 10);
  ring_buffer_ =
      std::make_unique<AudioRingBuffer>(runtime_capture_format_, queue_frames);

  if (!capture_adapter_->Start(config.capture, runtime_capture_format_, sink_)) {
    const auto capture_error = capture_adapter_->last_error();
    const auto error_stage =
        config.capture.source_mode == AudioSourceMode::ApplicationLoopback
            ? std::wstring(L"app-loopback-start")
            : std::wstring(L"capture-start");
    SetLastError(error_stage,
                 L"Failed to start capture adapter. " + capture_error);
    Log(L"Failed to start capture adapter.");
    return false;
  }

  if (config.render.monitor_enabled) {
    if (!render_adapter_->Start(effective_render_config, runtime_render_format_, sink_)) {
      capture_adapter_->Stop();
      SetLastError(L"render-start",
                   L"Failed to start render adapter. " + render_adapter_->last_error());
      Log(L"Failed to start render adapter.");
      return false;
    }
  }

  if (config.capture.dump_enabled) {
    std::filesystem::path dump_path = config.capture.dump_path.empty()
                                          ? BuildDefaultDumpPath()
                                          : std::filesystem::path(config.capture.dump_path);
    if (!dump_writer_->Open(dump_path, runtime_capture_format_,
                            config.capture.dump_file_type)) {
      SetLastError(L"dump-open", L"Failed to open dump file.");
      Log(L"Failed to open dump file: " + dump_path.wstring());
      diagnostics_.issues.push_back({L"Dump open failed", dump_path.wstring()});
    } else {
      diagnostics_.stats.dump_path = dump_path.wstring();
      Log(L"Dump file opened: " + dump_path.wstring());
    }
  }

  diagnostics_.stats.requested_capture_format =
      DescribeAudioFormat(config.capture.format);
  diagnostics_.stats.requested_render_format =
      DescribeAudioFormat(effective_render_config.format);
  diagnostics_.stats.active_render_monitor_enabled = config.render.monitor_enabled;
  diagnostics_.stats.active_requested_timing_present = true;
  diagnostics_.stats.active_requested_wasapi_mode_present = true;
  diagnostics_.stats.requested_capture_device_id =
      config.capture.device_id.empty() ? std::wstring(L"default")
                                       : config.capture.device_id;
  diagnostics_.stats.requested_render_device_id =
      config.render.device_id.empty() ? std::wstring(L"default")
                                      : config.render.device_id;
  diagnostics_.stats.requested_monitor_delay_ms = config.render.fixed_delay_ms;
  diagnostics_.stats.requested_capture_buffer_duration_ms =
      config.capture.buffer_duration_ms;
  diagnostics_.stats.requested_render_buffer_duration_ms =
      config.render.buffer_duration_ms;
  diagnostics_.stats.requested_capture_wasapi_mode =
      ToWideString(config.capture.wasapi_share_mode) + L" / " +
      ToWideString(config.capture.wasapi_drive_mode);
  diagnostics_.stats.requested_render_wasapi_mode =
      ToWideString(config.render.wasapi_share_mode) + L" / " +
      ToWideString(config.render.wasapi_drive_mode);
  diagnostics_.stats.negotiated_capture_format =
      DescribeAudioFormat(runtime_capture_format_);
  diagnostics_.stats.negotiated_render_format =
      DescribeAudioFormat(runtime_render_format_);
  diagnostics_.stats.actual_capture_backend_mode =
      capture_adapter_->runtime_mode().empty()
          ? ToWideString(config.capture.backend)
          : capture_adapter_->runtime_mode();
  diagnostics_.stats.actual_render_backend_mode =
      render_adapter_->runtime_mode().empty()
          ? ToWideString(config.render.backend)
          : render_adapter_->runtime_mode();
  diagnostics_.stats.actual_resampler_mode = resampler_->mode_name();
  diagnostics_.stats.capture_runtime_details = capture_adapter_->runtime_details();
  diagnostics_.stats.render_runtime_details = render_adapter_->runtime_details();

  capture_analyzer_.Reset();
  render_analyzer_.Reset();
  running_ = true;
  if (sink_ != nullptr) {
    sink_->OnSessionStateChanged(L"Running");
  }
  Log(L"Session started.");
  if (config.auto_align_render_format &&
      DescribeAudioFormat(config.render.format) !=
          diagnostics_.stats.requested_render_format) {
    Log(L"Render auto-align applied. Configured render format " +
        DescribeAudioFormat(config.render.format) + L", effective request " +
        diagnostics_.stats.requested_render_format);
  }
  if (diagnostics_.stats.requested_capture_format !=
      diagnostics_.stats.negotiated_capture_format) {
    Log(L"Capture format negotiated from requested " +
        diagnostics_.stats.requested_capture_format + L" to " +
        diagnostics_.stats.negotiated_capture_format);
  }
  if (diagnostics_.stats.requested_render_format !=
      diagnostics_.stats.negotiated_render_format) {
    Log(L"Render format negotiated from requested " +
        diagnostics_.stats.requested_render_format + L" to " +
        diagnostics_.stats.negotiated_render_format);
  }
  UpdateStats();
  return true;
}

void AudioSessionController::Stop() {
  if (capture_adapter_) {
    capture_adapter_->Stop();
  }
  if (render_adapter_) {
    render_adapter_->Stop();
  }
  if (dump_writer_) {
    dump_writer_->Close();
  }
  running_ = false;
  if (sink_ != nullptr) {
    sink_->OnSessionStateChanged(L"Stopped");
  }
}

bool AudioSessionController::Tick() {
  if (!running_ || !capture_adapter_ || !ring_buffer_) {
    return false;
  }

  const auto capture_chunk = capture_adapter_->ReadChunk();
  if (!capture_chunk.has_value()) {
    UpdateStats();
    return true;
  }

  ring_buffer_->Push(*capture_chunk);
  capture_analyzer_.Push(*capture_chunk);
  if (sink_ != nullptr) {
    sink_->OnWaveformUpdated(AudioDirection::Capture, capture_analyzer_.waveform(),
                             capture_analyzer_.meter());
  }

  if (dump_writer_ && dump_writer_->is_open()) {
    dump_writer_->Write(*capture_chunk);
  }

  if (!config_.render.monitor_enabled) {
    UpdateStats();
    return true;
  }

  const auto delay_frames = static_cast<uint32_t>(
      static_cast<uint64_t>(runtime_capture_format_.sample_rate) *
      config_.render.fixed_delay_ms / 1000);
  const auto delayed_chunk =
      ring_buffer_->PeekDelayedFrames(delay_frames, capture_chunk->frame_count());
  if (!delayed_chunk.has_value()) {
    UpdateStats();
    return true;
  }

  auto output_chunk = resampler_->Resample(*delayed_chunk);
  if (!output_chunk.has_value()) {
    SetLastError(L"resample", L"Resampler failed to produce output.");
    Log(L"Resampler failed to produce output.");
    diagnostics_.stats.render_underruns += 1;
    UpdateStats();
    return false;
  }

  render_analyzer_.Push(*output_chunk);
  diagnostics_.stats.render_wave_updates += 1;
  if (sink_ != nullptr) {
    sink_->OnWaveformUpdated(AudioDirection::Render, render_analyzer_.waveform(),
                             render_analyzer_.meter());
  }

  if (!render_adapter_ || !render_adapter_->WriteChunk(*output_chunk)) {
    const auto detail = render_adapter_ ? render_adapter_->last_error() : L"render adapter missing";
    SetLastError(L"render-write", L"Render adapter rejected audio chunk. " + detail);
    Log(L"Render adapter rejected audio chunk.");
    diagnostics_.stats.render_underruns += 1;
    UpdateStats();
    return false;
  }

  const auto consumed = ring_buffer_->PopFrames(capture_chunk->frame_count());
  if (!consumed.has_value()) {
    diagnostics_.stats.capture_overruns += 1;
  }

  UpdateStats();
  return true;
}

bool AudioSessionController::is_running() const {
  return running_;
}

const SessionDiagnostics& AudioSessionController::diagnostics() const {
  return diagnostics_;
}

void AudioSessionController::Log(const std::wstring& line) {
  diagnostics_.log_lines.push_back(line);
  if (sink_ != nullptr) {
    sink_->OnLogLine(line);
  }
}

void AudioSessionController::SetLastError(const std::wstring& stage,
                                          const std::wstring& message) {
  diagnostics_.stats.last_error_stage = stage;
  diagnostics_.stats.last_error_message = message;
}

void AudioSessionController::UpdateStats() {
  diagnostics_.stats.capture_meter = capture_analyzer_.meter();
  diagnostics_.stats.render_meter = render_analyzer_.meter();
  diagnostics_.stats.active_render_monitor_enabled = config_.render.monitor_enabled;
  diagnostics_.stats.queue_depth_frames =
      ring_buffer_ ? ring_buffer_->size_frames() : 0;
  diagnostics_.stats.queue_depth_ms =
      runtime_capture_format_.sample_rate > 0
          ? static_cast<uint32_t>(diagnostics_.stats.queue_depth_frames * 1000 /
                                  runtime_capture_format_.sample_rate)
          : 0;
  diagnostics_.stats.dropped_frames =
      ring_buffer_ ? ring_buffer_->dropped_frames() : 0;
  diagnostics_.stats.estimated_monitor_delay_ms =
      config_.render.fixed_delay_ms + diagnostics_.stats.queue_depth_ms;
  if (sink_ != nullptr) {
    sink_->OnStatsUpdated(diagnostics_.stats);
  }
}

std::filesystem::path AudioSessionController::BuildDefaultDumpPath() const {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time {};
  localtime_s(&local_time, &time);

  std::wstringstream file_name;
  file_name << L"winaudio_" << std::put_time(&local_time, L"%Y%m%d_%H%M%S")
            << L"_" << ToWideString(config_.capture.backend) << L"_"
            << ToWideString(config_.capture.source_mode) << L"_"
            << runtime_capture_format_.sample_rate << L"hz_"
            << runtime_capture_format_.channels << L"ch"
            << (config_.capture.dump_file_type == DumpFileType::Wav ? L".wav"
                                                                    : L".pcm");

  return std::filesystem::temp_directory_path() / file_name.str();
}

}  // namespace winaudio
