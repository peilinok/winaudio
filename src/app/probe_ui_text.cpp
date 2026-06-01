#include "app/probe_ui_text.h"

namespace winaudio {

std::wstring BuildWindowTitleFormatText(const std::wstring& negotiated_format,
                                        const std::wstring& configured_format) {
  if (!negotiated_format.empty()) {
    return negotiated_format;
  }
  if (!configured_format.empty()) {
    return configured_format;
  }
  return L"unavailable";
}

std::wstring BuildWindowTitleText(ProbeUiMode probe_mode,
                                  const std::wstring& session_state,
                                  const std::wstring& negotiated_capture_format,
                                  const std::wstring& negotiated_render_format) {
  std::wstring title = L"WinAudio Demo";
  if (probe_mode == ProbeUiMode::Quick) {
    title += L" | Quick Probe Running";
  } else if (probe_mode == ProbeUiMode::Matrix) {
    title += L" | Probe Matrix Running";
  } else {
    title += L" | " + session_state;
  }
  title += L" | Capture " + negotiated_capture_format +
           L" | Render " + negotiated_render_format;
  return title;
}

std::wstring BuildProbeButtonLabel(bool busy) {
  return busy ? L"Quick Probe Running..." : L"Run Quick Probe";
}

std::wstring BuildProbeMatrixButtonLabel(bool busy) {
  return busy ? L"Probe Matrix Running..." : L"Run Probe Matrix";
}

std::wstring BuildAutoAlignExplanatoryNoteText() {
  return L"When render auto-align is on, effective render format follows capture.";
}

std::wstring BuildEffectiveRenderRequestSummaryText(
    const std::wstring& effective_render_request) {
  if (effective_render_request.empty()) {
    return {};
  }
  return L"Effective render request: " + effective_render_request;
}

std::wstring BuildRunningDeviceSelectionDriftSummaryText(
    const std::wstring& session_state,
    bool follow_default_devices,
    const std::wstring& selected_capture_id,
    const std::wstring& selected_render_id,
    const std::wstring& active_requested_capture_id,
    const std::wstring& active_requested_render_id) {
  if (session_state != L"Running") {
    return {};
  }
  if (follow_default_devices) {
    return {};
  }
  const bool capture_drift =
      !selected_capture_id.empty() && !active_requested_capture_id.empty() &&
      selected_capture_id != active_requested_capture_id;
  const bool render_drift =
      !selected_render_id.empty() && !active_requested_render_id.empty() &&
      selected_render_id != active_requested_render_id;
  if (!capture_drift && !render_drift) {
    return {};
  }
  if (capture_drift && render_drift) {
    return L"Running session note: current capture/render device picks apply to the next rebuilt or restarted session, while the already-active stream still uses the previous devices.";
  }
  if (capture_drift) {
    return L"Running session note: the current capture device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous capture device.";
  }
  return L"Running session note: the current render device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous render device.";
}

std::wstring BuildCaptureDeviceLabelText(bool loopback_source) {
  return loopback_source ? L"Loopback Capture Device" : L"Capture Device";
}

std::wstring BuildCaptureDeviceLabelText(AudioSourceMode source_mode) {
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return L"Loopback Capture Device";
  }
  if (source_mode == AudioSourceMode::ApplicationLoopback) {
    return L"App Loopback Source";
  }
  return L"Capture Device";
}

std::wstring BuildDeviceCountLineText(bool loopback_source,
                                      size_t capture_device_count,
                                      size_t render_device_count) {
  if (loopback_source) {
    return L"Loopback capture devices: " +
           std::to_wstring(capture_device_count) +
           L" | Render devices: " + std::to_wstring(render_device_count);
  }
  return L"Capture devices: " + std::to_wstring(capture_device_count) +
         L" | Render devices: " + std::to_wstring(render_device_count);
}

std::wstring BuildDeviceCountLineText(AudioSourceMode source_mode,
                                      size_t capture_device_count,
                                      size_t render_device_count) {
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return BuildDeviceCountLineText(true, capture_device_count, render_device_count);
  }
  if (source_mode == AudioSourceMode::ApplicationLoopback) {
    return L"Application loopback sources: " +
           std::to_wstring(capture_device_count) + L" | Render devices: " +
           std::to_wstring(render_device_count);
  }
  return BuildDeviceCountLineText(false, capture_device_count, render_device_count);
}

std::wstring BuildLoopbackCaptureNoteText(bool loopback_source) {
  return loopback_source
             ? L"Loopback capture uses render endpoints as capture sources."
             : std::wstring {};
}

std::wstring BuildLoopbackCaptureNoteText(AudioSourceMode source_mode) {
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return L"Loopback capture uses render endpoints as capture sources.";
  }
  if (source_mode == AudioSourceMode::ApplicationLoopback) {
    return L"Application loopback captures audio rendered by a target process tree instead of a device endpoint.";
  }
  return {};
}

std::wstring BuildLoopbackBackendNoteText(bool loopback_source,
                                          bool wasapi_capture_backend) {
  if (!loopback_source) {
    return {};
  }
  return wasapi_capture_backend
             ? L"Loopback note: WASAPI loopback is shared-mode only"
             : L"Loopback note: WAVE loopback depends on hardware/driver devices";
}

std::wstring BuildLoopbackBackendNoteText(AudioSourceMode source_mode,
                                          bool wasapi_capture_backend) {
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return BuildLoopbackBackendNoteText(true, wasapi_capture_backend);
  }
  if (source_mode == AudioSourceMode::ApplicationLoopback) {
    return wasapi_capture_backend
               ? L"Application loopback note: process-tree loopback requires a WASAPI capture path and newer Windows support."
               : L"Application loopback note: WAVE capture does not support process-targeted loopback.";
  }
  return {};
}

std::wstring BuildFollowDefaultsNoteText(bool follow_defaults,
                                         bool loopback_source) {
  if (!follow_defaults) {
    return {};
  }
  if (loopback_source) {
    return L"Device selection follows current system defaults; manual device picks are inactive and loopback capture follows the current default render endpoint";
  }
  return L"Device selection follows current system defaults; manual device picks are inactive";
}

std::wstring BuildFollowDefaultsNoteText(bool follow_defaults,
                                         AudioSourceMode source_mode) {
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return BuildFollowDefaultsNoteText(follow_defaults, true);
  }
  if (source_mode == AudioSourceMode::ApplicationLoopback) {
    return follow_defaults
               ? L"Device selection follows current system defaults for render monitoring; app-loopback capture itself is process-targeted."
               : std::wstring {};
  }
  return BuildFollowDefaultsNoteText(follow_defaults, false);
}

std::wstring BuildFollowDefaultsDiagnosticsText(bool follow_defaults,
                                                bool loopback_source) {
  if (!follow_defaults) {
    return {};
  }
  if (loopback_source) {
    return L"Device tracking: current capture/render selection follows system defaults, and loopback capture follows the current default render endpoint";
  }
  return L"Device tracking: current capture/render selection follows system defaults";
}

std::wstring BuildFollowDefaultsDiagnosticsText(bool follow_defaults,
                                                AudioSourceMode source_mode) {
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return BuildFollowDefaultsDiagnosticsText(follow_defaults, true);
  }
  if (source_mode == AudioSourceMode::ApplicationLoopback) {
    return follow_defaults
               ? L"Device tracking: render monitoring follows system defaults while application loopback capture remains process-targeted"
               : std::wstring {};
  }
  return BuildFollowDefaultsDiagnosticsText(follow_defaults, false);
}

std::wstring BuildApplicationLoopbackTargetSummaryText(
    const std::wstring& target_process) {
  if (target_process.empty()) {
    return L"App loopback target: not set";
  }
  return L"App loopback target: " + target_process;
}

std::wstring BuildApplicationLoopbackNoteText(
    const std::wstring& target_process) {
  if (target_process.empty()) {
    return L"Application loopback needs a target process name or PID.";
  }
  return L"Application loopback is only available on supported Windows builds; unsupported machines will report that limitation before capture starts.";
}

std::wstring BuildApplicationLoopbackDiagnosticsText(
    const std::wstring& target_process) {
  if (target_process.empty()) {
    return L"Application loopback target process: not set";
  }
  return L"Application loopback target process: " + target_process;
}

std::wstring BuildMonitorDisabledNoteText(bool configured_monitor_enabled,
                                          bool active_render_monitor_enabled,
                                          const std::wstring& session_state) {
  if (configured_monitor_enabled) {
    return {};
  }
  if (session_state == L"Running" && active_render_monitor_enabled) {
    return L"Render monitor playback is turned off for the next rebuilt or restarted session; the already-active render stream is still running.";
  }
  return L"Render pipeline disabled for monitoring; render-only settings are inactive";
}

std::wstring BuildMonitorDisabledDiagnosticsText(
    bool configured_monitor_enabled,
    bool active_render_monitor_enabled,
    const std::wstring& session_state) {
  if (configured_monitor_enabled) {
    return {};
  }
  if (session_state == L"Running" && active_render_monitor_enabled) {
    return L"Render monitor playback is disabled in the current configuration, but the already-active session still has render monitoring until the next rebuild or restart.";
  }
  return L"Render pipeline disabled: render-only settings are inactive while monitor playback is off";
}

std::wstring BuildMonitorDisabledRenderWaveNoteText(bool monitor_enabled) {
  return monitor_enabled
             ? std::wstring {}
             : L"render monitoring is disabled because monitor playback is off";
}

std::wstring BuildRunningSessionConfigurationNoteText(
    const std::wstring& session_state) {
  if (session_state != L"Running") {
    return {};
  }
  return L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.";
}

std::wstring BuildRunningDeviceChangeSummaryText(
    const std::wstring& session_state,
    const std::wstring& reason,
    const std::wstring& result) {
  if (session_state != L"Running" || reason.empty() || result.empty()) {
    return {};
  }
  if (reason == L"default-device-change" && result == L"tracked-no-rebuild") {
    return L"Running session note: the default-device-change was tracked, but the already-active session kept its current devices because follow-defaults is off.";
  }
  if ((reason == L"default-device-change" || reason == L"refresh-devices") &&
      result == L"rebuild-success") {
    return L"Running session note: the active session was rebuilt successfully after the device change.";
  }
  return {};
}

std::wstring BuildCurrentConfiguredCaptureDiagnosticsLabelText() {
  return L"Current configured capture: ";
}

std::wstring BuildCurrentConfiguredRenderDiagnosticsLabelText() {
  return L"Current configured render: ";
}

std::wstring BuildEffectiveConfiguredRenderRequestDiagnosticsLabelText() {
  return L"Effective configured render request: ";
}

std::wstring BuildActiveRequestedCaptureDiagnosticsLabelText() {
  return L"Active session requested capture: ";
}

std::wstring BuildActiveRequestedRenderDiagnosticsLabelText() {
  return L"Active session requested render: ";
}

std::wstring BuildActiveRequestedCaptureDeviceIdDiagnosticsLabelText() {
  return L"Active session requested capture id: ";
}

std::wstring BuildActiveRequestedRenderDeviceIdDiagnosticsLabelText() {
  return L"Active session requested render id: ";
}

std::wstring BuildActiveNegotiatedCaptureDiagnosticsLabelText() {
  return L"Active session negotiated capture: ";
}

std::wstring BuildActiveNegotiatedRenderDiagnosticsLabelText() {
  return L"Active session negotiated render: ";
}

std::wstring BuildActiveCaptureModeDiagnosticsLabelText() {
  return L"Active capture mode: ";
}

std::wstring BuildActiveRenderModeDiagnosticsLabelText() {
  return L"Active render mode: ";
}

std::wstring BuildActiveResamplerDiagnosticsLabelText() {
  return L"Active resampler: ";
}

std::wstring BuildActiveCaptureRuntimeDiagnosticsLabelText() {
  return L"Active capture runtime: ";
}

std::wstring BuildActiveRenderRuntimeDiagnosticsLabelText() {
  return L"Active render runtime: ";
}

std::wstring BuildActiveCaptureWasapiRequestDiagnosticsLabelText() {
  return L"Active capture WASAPI request: ";
}

std::wstring BuildActiveRenderWasapiRequestDiagnosticsLabelText() {
  return L"Active render WASAPI request: ";
}

std::wstring BuildActiveMonitorDelayDiagnosticsLabelText() {
  return L"Active monitor delay: ";
}

std::wstring BuildActiveCaptureBufferDiagnosticsLabelText() {
  return L"Active capture buffer: ";
}

std::wstring BuildActiveRenderBufferDiagnosticsLabelText() {
  return L"Active render buffer: ";
}

std::wstring BuildLastDeviceChangeDiagnosticsText(
    const std::wstring& reason,
    const std::wstring& result) {
  if (reason.empty()) {
    return {};
  }
  return L"Last device change: " + reason + L" => " + result;
}

std::wstring BuildLastRebuildDiagnosticsText(const std::wstring& reason,
                                             const std::wstring& result) {
  if (reason.empty()) {
    return {};
  }
  return L"Last rebuild: " + reason + L" => " + result;
}

std::wstring BuildSelectedCaptureDeviceDiagnosticsLabelText(
    bool loopback_source) {
  return loopback_source ? L"Selected loopback capture device: "
                         : L"Selected capture device: ";
}

std::wstring BuildSelectedCaptureDeviceDiagnosticsLabelText(
    AudioSourceMode source_mode) {
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return L"Selected loopback capture device: ";
  }
  if (source_mode == AudioSourceMode::ApplicationLoopback) {
    return L"Selected app-loopback source: ";
  }
  return L"Selected capture device: ";
}

std::wstring BuildSelectedCaptureDeviceIdDiagnosticsLabelText(
    bool loopback_source) {
  return loopback_source ? L"Selected loopback capture id: "
                         : L"Selected capture id: ";
}

std::wstring BuildSelectedCaptureDeviceIdDiagnosticsLabelText(
    AudioSourceMode source_mode) {
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return L"Selected loopback capture id: ";
  }
  if (source_mode == AudioSourceMode::ApplicationLoopback) {
    return L"Selected app-loopback id: ";
  }
  return L"Selected capture id: ";
}

}  // namespace winaudio
