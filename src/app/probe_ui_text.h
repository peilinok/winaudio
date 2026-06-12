#pragma once

#include <string>

#include "audio/audio_session_types.h"
#include "audio/audio_types.h"
#include "rtc/agora_rtc_types.h"

namespace winaudio {

enum class ProbeUiMode {
  None,
  Quick,
  Matrix,
  CaptureOpen,
};

std::wstring BuildWindowTitleText(ProbeUiMode probe_mode,
                                  const std::wstring& session_state,
                                  const std::wstring& negotiated_capture_format,
                                  const std::wstring& negotiated_render_format);
std::wstring BuildWindowTitleFormatText(const std::wstring& negotiated_format,
                                        const std::wstring& configured_format);
std::wstring BuildProbeButtonLabel(bool busy);
std::wstring BuildProbeMatrixButtonLabel(bool busy);
std::wstring BuildCaptureOpenProbeButtonLabel(bool busy);
std::wstring BuildAutoAlignExplanatoryNoteText();
std::wstring BuildEffectiveRenderRequestSummaryText(
    const std::wstring& effective_render_request);
std::wstring BuildRunningDeviceSelectionDriftSummaryText(
    const std::wstring& session_state,
    bool follow_default_devices,
    const std::wstring& selected_capture_id,
    const std::wstring& selected_render_id,
    const std::wstring& active_requested_capture_id,
    const std::wstring& active_requested_render_id);
std::wstring BuildCaptureDeviceLabelText(bool loopback_source);
std::wstring BuildCaptureDeviceLabelText(AudioSourceMode source_mode);
std::wstring BuildDeviceCountLineText(bool loopback_source,
                                      size_t capture_device_count,
                                      size_t render_device_count);
std::wstring BuildDeviceCountLineText(AudioSourceMode source_mode,
                                      size_t capture_device_count,
                                      size_t render_device_count);
std::wstring BuildLoopbackCaptureNoteText(bool loopback_source);
std::wstring BuildLoopbackCaptureNoteText(AudioSourceMode source_mode);
std::wstring BuildLoopbackBackendNoteText(bool loopback_source,
                                          bool wasapi_capture_backend);
std::wstring BuildLoopbackBackendNoteText(AudioSourceMode source_mode,
                                          bool wasapi_capture_backend);
std::wstring BuildFollowDefaultsNoteText(bool follow_defaults,
                                         bool loopback_source);
std::wstring BuildFollowDefaultsNoteText(bool follow_defaults,
                                         AudioSourceMode source_mode);
std::wstring BuildFollowDefaultsDiagnosticsText(bool follow_defaults,
                                                bool loopback_source);
std::wstring BuildFollowDefaultsDiagnosticsText(bool follow_defaults,
                                                AudioSourceMode source_mode);
std::wstring BuildApplicationLoopbackTargetSummaryText(
    ApplicationLoopbackTargetKind target_kind,
    const std::wstring& target_value);
std::wstring BuildApplicationLoopbackNoteText(
    ApplicationLoopbackTargetKind target_kind,
    const std::wstring& target_value);
std::wstring BuildApplicationLoopbackDiagnosticsText(
    ApplicationLoopbackTargetKind target_kind,
    const std::wstring& target_value);
std::wstring BuildMonitorDisabledNoteText(bool configured_monitor_enabled,
                                          bool active_render_monitor_enabled,
                                          const std::wstring& session_state);
std::wstring BuildMonitorDisabledDiagnosticsText(
    bool configured_monitor_enabled,
    bool active_render_monitor_enabled,
    const std::wstring& session_state);
std::wstring BuildMonitorDisabledRenderWaveNoteText(bool monitor_enabled);
std::wstring BuildRunningSessionConfigurationNoteText(
    const std::wstring& session_state);
std::wstring BuildRunningDeviceChangeSummaryText(
    const std::wstring& session_state,
    const std::wstring& reason,
    const std::wstring& result);
std::wstring BuildCurrentConfiguredCaptureDiagnosticsLabelText();
std::wstring BuildCurrentConfiguredRenderDiagnosticsLabelText();
std::wstring BuildEffectiveConfiguredRenderRequestDiagnosticsLabelText();
std::wstring BuildActiveRequestedCaptureDiagnosticsLabelText();
std::wstring BuildActiveRequestedRenderDiagnosticsLabelText();
std::wstring BuildActiveRequestedCaptureDeviceIdDiagnosticsLabelText();
std::wstring BuildActiveRequestedRenderDeviceIdDiagnosticsLabelText();
std::wstring BuildActiveNegotiatedCaptureDiagnosticsLabelText();
std::wstring BuildActiveNegotiatedRenderDiagnosticsLabelText();
std::wstring BuildActiveCaptureModeDiagnosticsLabelText();
std::wstring BuildActiveRenderModeDiagnosticsLabelText();
std::wstring BuildActiveResamplerDiagnosticsLabelText();
std::wstring BuildActiveCaptureRuntimeDiagnosticsLabelText();
std::wstring BuildActiveRenderRuntimeDiagnosticsLabelText();
std::wstring BuildActiveCaptureWasapiRequestDiagnosticsLabelText();
std::wstring BuildActiveRenderWasapiRequestDiagnosticsLabelText();
std::wstring BuildActiveMonitorDelayDiagnosticsLabelText();
std::wstring BuildActiveCaptureBufferDiagnosticsLabelText();
std::wstring BuildActiveRenderBufferDiagnosticsLabelText();
std::wstring BuildLastDeviceChangeDiagnosticsText(
    const std::wstring& reason,
    const std::wstring& result);
std::wstring BuildLastRebuildDiagnosticsText(const std::wstring& reason,
                                             const std::wstring& result);
std::wstring BuildSelectedCaptureDeviceDiagnosticsLabelText(
    bool loopback_source);
std::wstring BuildSelectedCaptureDeviceDiagnosticsLabelText(
    AudioSourceMode source_mode);
std::wstring BuildSelectedCaptureDeviceIdDiagnosticsLabelText(
    bool loopback_source);
std::wstring BuildSelectedCaptureDeviceIdDiagnosticsLabelText(
    AudioSourceMode source_mode);

std::wstring BuildRtcJoinStatusText(const AgoraRtcStats& stats,
                                    bool rtc_enabled,
                                    const std::wstring& session_state);
std::wstring BuildRtcAvailabilityText(const AgoraRtcRuntimeStatus& runtime_status);
std::wstring BuildRtcAvailabilityCodeText(const AgoraRtcRuntimeStatus& runtime_status);
std::wstring BuildRtcDisableReasonText(const AgoraRtcRuntimeStatus& runtime_status);
std::wstring BuildRtcJoinButtonLabelText(const AgoraRtcConfig& config,
                                         const AgoraRtcStats& stats);
std::wstring BuildRtcStatusLabelText(const SessionConfiguration& config,
                                     const std::wstring& session_state,
                                     const AgoraRtcStats& stats);
bool IsRtcRuntimeAvailable(const AgoraRtcStats& stats);
std::wstring BuildRtcText(const AgoraRtcConfig& config,
                          const AgoraRtcStats& stats,
                          const std::wstring& session_state);
bool IsRtcCliSessionReady(const AgoraRtcStats& stats);
bool HasRtcCliSessionFailed(const AgoraRtcStats& stats);
std::wstring BuildRtcCapabilitySummaryText(
    const AgoraRtcRuntimeStatus& runtime_status);
std::wstring BuildRtcLimitationText(const AgoraRtcRuntimeStatus& runtime_status);

}  // namespace winaudio
