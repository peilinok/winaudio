#include "audio_session_controller.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "audio/backends/real_backends.h"

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
    return L"Application loopback requires a target process id or application name.";
  }
  if (detail == L"app-loopback-process-id-required") {
    return L"Application process loopback requires a target process id.";
  }
  if (detail == L"app-loopback-application-required") {
    return L"Application loopback requires a target application name.";
  }
  if (detail == L"app-loopback-invalid-target") {
    return L"Application loopback target process was not found.";
  }
  if (detail == L"app-loopback-no-active-audio-session") {
    return L"Application loopback target application was found, but no active audio-playing process was available.";
  }
  if (detail == L"app-loopback-unsupported-os") {
    return WasapiCaptureAdapter::DescribeProcessLoopbackSupport();
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

std::wstring HumanizeFormatResolutionFailure(const std::wstring& detail) {
  if (detail == L"resolve-device") {
    return L"target device could not be resolved";
  }
  if (detail == L"activate-iaudioclient") {
    return L"IAudioClient activation failed";
  }
  if (detail == L"wave-loopback-device-not-found") {
    return L"loopback device was not found";
  }
  return detail;
}

std::wstring HumanizeCaptureStartFailure(const std::wstring& detail) {
  if (detail == L"resolve-device") {
    return L"target capture device could not be resolved at start time";
  }
  if (detail == L"activate-iaudioclient") {
    return L"IAudioClient activation failed while starting capture";
  }
  if (detail == L"query-iaudioclient2") {
    return L"IAudioClient2 is unavailable, so client properties could not be applied";
  }
  if (detail.find(L"SetClientProperties: AUDCLNT_E_UNSUPPORTED_FORMAT") !=
      std::wstring::npos) {
    return L"WASAPI client properties or stream options were rejected for this capture path";
  }
  if (detail.find(L"SetClientProperties:") == 0) {
    return L"WASAPI client properties were rejected while starting capture";
  }
  if (detail.find(L"Initialize: AUDCLNT_E_UNSUPPORTED_FORMAT") == 0) {
    return L"The requested capture format was not accepted by the device or share mode";
  }
  if (detail.find(L"Initialize: AUDCLNT_E_DEVICE_INVALIDATED") == 0) {
    return L"The capture device became unavailable while the stream was being initialized";
  }
  if (detail.find(L"Initialize: AUDCLNT_E_WRONG_ENDPOINT_TYPE") == 0) {
    return L"The selected endpoint type does not match the requested capture path";
  }
  if (detail.find(L"Initialize: AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED") == 0) {
    return L"The requested WASAPI buffer size was not aligned for this device";
  }
  if (detail.find(L"Initialize:") == 0) {
    return L"WASAPI stream initialization failed for the negotiated capture format";
  }
  if (detail.find(L"GetService(IAudioCaptureClient)") == 0) {
    return L"IAudioCaptureClient service acquisition failed after stream initialization";
  }
  if (detail.find(L"Start: AUDCLNT_E_DEVICE_INVALIDATED") == 0) {
    return L"The capture device became unavailable when the stream was starting";
  }
  if (detail.find(L"Start:") == 0) {
    return L"The capture stream could not be started after initialization";
  }
  if (detail == L"wave-loopback-device-not-found") {
    return L"the requested loopback capture device was not available";
  }
  if (detail.find(L"waveInOpen: WAVERR_BADFORMAT") == 0) {
    return L"The negotiated WAVE input format was rejected by the driver";
  }
  if (detail.find(L"waveInOpen: MMSYSERR_ALLOCATED") == 0) {
    return L"The WAVE input device is already in use by another client";
  }
  if (detail.find(L"waveInOpen: MMSYSERR_BADDEVICEID") == 0) {
    return L"The WAVE input device id is invalid";
  }
  if (detail.find(L"waveInOpen:") == 0) {
    return L"waveInOpen failed for the negotiated capture format";
  }
  if (detail == L"waveInPrepareHeader/waveInAddBuffer") {
    return L"The WAVE input buffers could not be prepared or queued";
  }
  if (detail.find(L"waveInStart:") == 0) {
    return L"The WAVE input stream could not be started after opening the device";
  }
  return {};
}

std::wstring BuildCaptureStartFailureDetail(
    const CaptureConfig& config,
    const AudioFormatSpec& runtime_format,
    const std::wstring& requested_format_text,
    const std::wstring& negotiated_format_text,
    const std::wstring& capture_error) {
  std::wstring detail = L"Failed to start capture adapter.";
  detail += L" backend=" + ToWideString(config.backend);
  detail += L", source=" + ToWideString(config.source_mode);
  detail += L", device=" +
            (config.device_id.empty() ? std::wstring(L"default")
                                      : config.device_id);
  detail += L", requested=" + requested_format_text;
  detail += L", negotiated=" +
            (negotiated_format_text.empty() ? DescribeAudioFormat(runtime_format)
                                            : negotiated_format_text);
  detail += L", wasapi=" + ToWideString(config.wasapi_share_mode) + L" / " +
            ToWideString(config.wasapi_drive_mode) + L" / " +
            ToWideString(config.wasapi_stream_category) + L" / " +
            ToWideString(config.wasapi_stream_options);
  if (config.source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      config.source_mode == AudioSourceMode::ApplicationLoopback) {
    detail += L", app-target=" +
              (config.application_loopback_target_value.empty()
                   ? std::wstring(L"<empty>")
                   : config.application_loopback_target_value);
  }
  if (!capture_error.empty()) {
    detail += L". backend-detail=" + capture_error;
    const auto humanized = HumanizeCaptureStartFailure(capture_error);
    if (!humanized.empty()) {
      detail += L". hint=" + humanized;
    }
    const auto app_loopback_reason = HumanizeAppLoopbackFailure(capture_error);
    if (!app_loopback_reason.empty()) {
      detail += L". loopback=" + app_loopback_reason;
    }
  }
  return detail;
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
  if (config_.capture.source_mode == AudioSourceMode::SystemLoopback &&
      config_.render.monitor_enabled) {
    config_.render.monitor_enabled = false;
    Log(L"System loopback disables monitor playback to avoid loopback storm.");
  }
  sink_ = sink;
  const auto& effective_config = config_;

  capture_adapter_ =
      backend_factory_->CreateCaptureAdapter(effective_config.capture.backend);
  render_adapter_ =
      backend_factory_->CreateRenderAdapter(effective_config.render.backend);
  resampler_ = CreateAudioResampler();
  dump_writer_ = std::make_unique<WavDumpWriter>();
  rtc_publisher_ = CreateAgoraRtcPublisher();

  if (!capture_adapter_ || !render_adapter_ || !resampler_ || !rtc_publisher_) {
    SetLastError(L"create-components", L"Failed to create one or more audio components.");
    Log(L"Failed to create one or more audio components.");
    return false;
  }

  if (!capture_adapter_->SupportsSource(effective_config.capture.source_mode)) {
    std::wstring detail;
    if (effective_config.capture.source_mode == AudioSourceMode::SystemLoopback) {
      detail =
          L"Selected backend does not support the chosen capture source. Use --capture-backend=wasapi for loopback, or switch --source=mic.";
    } else if (effective_config.capture.source_mode ==
                   AudioSourceMode::ApplicationProcessLoopback ||
               effective_config.capture.source_mode ==
                   AudioSourceMode::ApplicationLoopback) {
      detail =
          L"Selected backend does not support application loopback. Use --capture-backend=wasapi and provide an application loopback target on a supported Windows build.";
    } else {
      detail = L"Selected backend does not support the chosen capture source.";
    }
    diagnostics_.issues.push_back(
        {L"Source mode unsupported", detail});
    SetLastError(L"source-mode", detail);
    Log(L"Capture backend does not support the selected source mode.");
    return false;
  }

  if ((effective_config.capture.source_mode ==
           AudioSourceMode::ApplicationProcessLoopback ||
       effective_config.capture.source_mode ==
           AudioSourceMode::ApplicationLoopback) &&
      effective_config.capture.application_loopback_target_value.empty()) {
    const auto detail = effective_config.capture.source_mode ==
                                AudioSourceMode::ApplicationProcessLoopback
                            ? std::wstring(
                                  L"Application process loopback requires a target process id before starting capture.")
                            : std::wstring(
                                  L"Application loopback requires a target application name before starting capture.");
    diagnostics_.issues.push_back({L"Application loopback target missing", detail});
    SetLastError(L"source-mode", detail);
    Log(L"Application loopback target process is missing.");
    return false;
  }

  if (!effective_config.capture.device_id.empty()) {
    const auto capture_devices =
        capture_adapter_->EnumerateDevices(effective_config.capture.source_mode);
    if (!ContainsDeviceId(capture_devices, effective_config.capture.device_id)) {
      std::wstring detail;
      if (effective_config.capture.source_mode == AudioSourceMode::SystemLoopback) {
        detail =
            L"Selected loopback capture device is not available for this source. Use devices --source=loopback to choose a render-backed loopback endpoint.";
      } else if (effective_config.capture.source_mode ==
                     AudioSourceMode::ApplicationProcessLoopback ||
                 effective_config.capture.source_mode ==
                     AudioSourceMode::ApplicationLoopback) {
        detail =
            L"Selected application loopback source is not available for this configuration. Use the built-in app-loopback source entry and provide an application loopback target.";
      } else {
        detail =
            L"Selected capture device is not available for this backend/source mode. Choose a device from devices for the same capture backend/source, or omit --capture-device-id.";
      }
      diagnostics_.issues.push_back({L"Capture device unavailable", detail});
      SetLastError((effective_config.capture.source_mode ==
                        AudioSourceMode::SystemLoopback ||
                    effective_config.capture.source_mode ==
                        AudioSourceMode::ApplicationProcessLoopback ||
                    effective_config.capture.source_mode ==
                        AudioSourceMode::ApplicationLoopback)
                       ? L"source-mode"
                       : L"capture-device",
                   detail);
      Log(L"Selected capture device is not available for the requested source.");
      return false;
    }
  }

  if (effective_config.render.monitor_enabled &&
      !effective_config.render.device_id.empty()) {
    const auto render_devices = render_adapter_->EnumerateDevices();
    if (!ContainsDeviceId(render_devices, effective_config.render.device_id)) {
      const auto detail =
          std::wstring(
              L"Selected render device is not available for this backend. Choose a device from devices for the same render backend, or omit --render-device-id.");
      diagnostics_.issues.push_back({L"Render device unavailable", detail});
      SetLastError(L"render-device", detail);
      Log(L"Selected render device is not available for the requested backend.");
      return false;
    }
  }

  const auto capture_format =
      capture_adapter_->GetPreferredFormat(effective_config.capture);
  RenderConfig effective_render_config = effective_config.render;
  if (capture_format.has_value() && effective_config.auto_align_render_format) {
    effective_render_config.format = *capture_format;
    effective_render_config.format.normalize();
  }
  const auto render_format =
      effective_config.render.monitor_enabled
          ? render_adapter_->GetPreferredFormat(effective_render_config)
          : std::optional<AudioFormatSpec> {effective_render_config.format};

  diagnostics_.stats.requested_capture_format =
      DescribeAudioFormat(effective_config.capture.format);
  diagnostics_.stats.requested_render_format =
      DescribeAudioFormat(effective_render_config.format);
  diagnostics_.stats.effective_render_request_format =
      diagnostics_.stats.requested_render_format;
  diagnostics_.stats.active_render_monitor_enabled =
      effective_config.render.monitor_enabled;
  diagnostics_.stats.active_requested_timing_present = true;
  diagnostics_.stats.active_requested_wasapi_mode_present = true;
  diagnostics_.stats.requested_capture_device_id =
      effective_config.capture.device_id.empty() ? std::wstring(L"default")
                                                 : effective_config.capture.device_id;
  diagnostics_.stats.requested_render_device_id =
      effective_config.render.device_id.empty() ? std::wstring(L"default")
                                                : effective_config.render.device_id;
  diagnostics_.stats.requested_monitor_delay_ms =
      effective_config.render.fixed_delay_ms;
  diagnostics_.stats.requested_capture_buffer_duration_ms =
      effective_config.capture.buffer_duration_ms;
  diagnostics_.stats.requested_render_buffer_duration_ms =
      effective_config.render.buffer_duration_ms;
  diagnostics_.stats.requested_capture_wasapi_mode =
      ToWideString(effective_config.capture.wasapi_share_mode) + L" / " +
      ToWideString(effective_config.capture.wasapi_drive_mode) + L" / " +
      ToWideString(effective_config.capture.wasapi_stream_category) + L" / " +
      ToWideString(effective_config.capture.wasapi_stream_options);
  diagnostics_.stats.requested_render_wasapi_mode =
      ToWideString(effective_config.render.wasapi_share_mode) + L" / " +
      ToWideString(effective_config.render.wasapi_drive_mode) + L" / " +
      ToWideString(effective_config.render.wasapi_stream_category) + L" / " +
      ToWideString(effective_config.render.wasapi_stream_options);
  if (capture_format.has_value()) {
    diagnostics_.stats.negotiated_capture_format =
        DescribeAudioFormat(*capture_format);
  }
  if (render_format.has_value()) {
    diagnostics_.stats.negotiated_render_format =
        DescribeAudioFormat(*render_format);
  }

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
    if (effective_config.render.monitor_enabled && !render_format.has_value()) {
      const auto render_detail = render_adapter_->last_error();
      if (!render_detail.empty()) {
        detail += L" render=" + HumanizeFormatResolutionFailure(render_detail);
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
      runtime_capture_format_.sample_rate *
              effective_config.render.fixed_delay_ms / 1000 +
          runtime_capture_format_.sample_rate / 10);
  ring_buffer_ =
      std::make_unique<AudioRingBuffer>(runtime_capture_format_, queue_frames);

  if (!capture_adapter_->Start(effective_config.capture, runtime_capture_format_,
                               sink_)) {
    const auto capture_error = capture_adapter_->last_error();
    const auto error_stage =
        (effective_config.capture.source_mode ==
             AudioSourceMode::ApplicationProcessLoopback ||
         effective_config.capture.source_mode ==
             AudioSourceMode::ApplicationLoopback)
            ? std::wstring(L"app-loopback-start")
            : std::wstring(L"capture-start");
    SetLastError(error_stage,
                 BuildCaptureStartFailureDetail(
                     effective_config.capture, runtime_capture_format_,
                     diagnostics_.stats.requested_capture_format,
                     diagnostics_.stats.negotiated_capture_format, capture_error));
    Log(L"Failed to start capture adapter.");
    return false;
  }

  if (effective_config.render.monitor_enabled) {
    if (!render_adapter_->Start(effective_render_config, runtime_render_format_, sink_)) {
      capture_adapter_->Stop();
      SetLastError(L"render-start",
                   L"Failed to start render adapter. " + render_adapter_->last_error());
      Log(L"Failed to start render adapter.");
      return false;
    }
  }

  if (effective_config.rtc.enabled &&
      !JoinRtc(effective_config.rtc)) {
    if (effective_config.render.monitor_enabled) {
      render_adapter_->Stop();
    }
    capture_adapter_->Stop();
    const auto rtc_stats = rtc_publisher_->stats();
    SetLastError(
        rtc_stats.last_error_stage.empty() ? L"rtc-start"
                                           : rtc_stats.last_error_stage,
        rtc_stats.last_error_message.empty()
            ? std::wstring(L"Failed to start RTC publisher.")
            : rtc_stats.last_error_message);
    Log(L"Failed to start RTC publisher.");
    return false;
  }

  if (effective_config.capture.dump_enabled) {
    std::filesystem::path dump_path = effective_config.capture.dump_path.empty()
                                          ? BuildDefaultDumpPath()
                                          : std::filesystem::path(
                                                effective_config.capture.dump_path);
    if (!dump_writer_->Open(dump_path, runtime_capture_format_,
                            effective_config.capture.dump_file_type)) {
      SetLastError(L"dump-open", L"Failed to open dump file.");
      Log(L"Failed to open dump file: " + dump_path.wstring());
      diagnostics_.issues.push_back({L"Dump open failed", dump_path.wstring()});
    } else {
      diagnostics_.stats.dump_path = dump_path.wstring();
      Log(L"Dump file opened: " + dump_path.wstring());
    }
  }

  diagnostics_.stats.negotiated_capture_format =
      DescribeAudioFormat(runtime_capture_format_);
  diagnostics_.stats.negotiated_render_format =
      DescribeAudioFormat(runtime_render_format_);
  diagnostics_.stats.actual_capture_backend_mode =
      capture_adapter_->runtime_mode().empty()
          ? ToWideString(effective_config.capture.backend)
          : capture_adapter_->runtime_mode();
  diagnostics_.stats.actual_render_backend_mode =
      render_adapter_->runtime_mode().empty()
          ? ToWideString(effective_config.render.backend)
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
  if (effective_config.auto_align_render_format &&
      DescribeAudioFormat(effective_config.render.format) !=
          diagnostics_.stats.requested_render_format) {
    Log(L"Render auto-align applied. Configured render format " +
        DescribeAudioFormat(effective_config.render.format) +
        L", effective request " +
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
  if (rtc_publisher_) {
    rtc_publisher_->Stop();
  }
  running_ = false;
  if (sink_ != nullptr) {
    sink_->OnSessionStateChanged(L"Stopped");
  }
}

bool AudioSessionController::JoinRtc(const AgoraRtcConfig& config) {
  if (runtime_capture_format_.sample_rate == 0 || !rtc_publisher_) {
    return false;
  }
  config_.rtc = config;
  if (!rtc_publisher_->Initialize(config_.rtc)) {
    const auto rtc_stats = rtc_publisher_->stats();
    SetLastError(L"rtc-init", rtc_stats.last_error_message);
    Log(L"Failed to initialize RTC publisher.");
    return false;
  }
  if (!rtc_publisher_->Start(runtime_capture_format_)) {
    const auto rtc_stats = rtc_publisher_->stats();
    SetLastError(
        rtc_stats.last_error_stage.empty() ? L"rtc-start"
                                           : rtc_stats.last_error_stage,
        rtc_stats.last_error_message.empty()
            ? std::wstring(L"Failed to start RTC publisher.")
            : rtc_stats.last_error_message);
    Log(L"Failed to start RTC publisher.");
    return false;
  }
  Log(L"RTC channel join requested.");
  return true;
}

void AudioSessionController::LeaveRtc() {
  config_.rtc.enabled = false;
  if (rtc_publisher_) {
    rtc_publisher_->Stop();
  }
  Log(L"RTC channel leave requested.");
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

  if (config_.rtc.enabled && rtc_publisher_ != nullptr &&
      rtc_publisher_->stats().joined) {
    if (!rtc_publisher_->PublishChunk(*capture_chunk)) {
      const auto rtc_stats = rtc_publisher_->stats();
      SetLastError(
          rtc_stats.last_error_stage.empty() ? L"rtc-publish"
                                             : rtc_stats.last_error_stage,
          rtc_stats.last_error_message.empty()
              ? std::wstring(L"RTC publisher rejected audio chunk.")
              : rtc_stats.last_error_message);
      Log(L"RTC publisher rejected audio chunk.");
      UpdateStats();
      return false;
    }
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

AgoraRtcStats AudioSessionController::rtc_stats() const {
  if (!rtc_publisher_) {
    return {};
  }
  return rtc_publisher_->stats();
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
