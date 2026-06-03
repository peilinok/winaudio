#include <iostream>
#include <vector>

#include "app/probe_ui_text.h"

using namespace winaudio;

namespace {

bool TestBuildWindowTitleIdle() {
  const auto title =
      BuildWindowTitleText(ProbeUiMode::None, L"Running", L"48000 Hz / 2 ch / PCM16",
                           L"48000 Hz / 2 ch / PCM16");
  return title ==
         L"WinAudio Demo | Running | Capture 48000 Hz / 2 ch / PCM16 | Render 48000 Hz / 2 ch / PCM16";
}

bool TestBuildWindowTitleFormatText() {
  return BuildWindowTitleFormatText(L"48000 Hz / 2 ch / PCM16",
                                    L"44100 Hz / 1 ch / PCM24") ==
             L"48000 Hz / 2 ch / PCM16" &&
         BuildWindowTitleFormatText(L"", L"44100 Hz / 1 ch / PCM24") ==
             L"44100 Hz / 1 ch / PCM24" &&
         BuildWindowTitleFormatText(L"", L"") == L"unavailable";
}

bool TestBuildWindowTitleQuickBusy() {
  const auto title =
      BuildWindowTitleText(ProbeUiMode::Quick, L"Running", L"44100 Hz / 1 ch / PCM16",
                           L"48000 Hz / 2 ch / Float32");
  return title ==
         L"WinAudio Demo | Quick Probe Running | Capture 44100 Hz / 1 ch / PCM16 | Render 48000 Hz / 2 ch / Float32";
}

bool TestBuildWindowTitleMatrixBusy() {
  const auto title =
      BuildWindowTitleText(ProbeUiMode::Matrix, L"Running", L"44100 Hz / 1 ch / PCM16",
                           L"48000 Hz / 2 ch / Float32");
  return title ==
         L"WinAudio Demo | Probe Matrix Running | Capture 44100 Hz / 1 ch / PCM16 | Render 48000 Hz / 2 ch / Float32";
}

bool TestBuildProbeButtonLabel() {
  return BuildProbeButtonLabel(false) == L"Run Quick Probe" &&
         BuildProbeButtonLabel(true) == L"Quick Probe Running...";
}

bool TestBuildProbeMatrixButtonLabel() {
  return BuildProbeMatrixButtonLabel(false) == L"Run Probe Matrix" &&
         BuildProbeMatrixButtonLabel(true) == L"Probe Matrix Running...";
}

bool TestBuildAutoAlignExplanatoryNoteText() {
  return BuildAutoAlignExplanatoryNoteText() ==
         L"When render auto-align is on, effective render format follows capture.";
}

bool TestBuildEffectiveRenderRequestSummaryText() {
  return BuildEffectiveRenderRequestSummaryText(L"").empty() &&
         BuildEffectiveRenderRequestSummaryText(L"44100 Hz / 1 ch / PCM16") ==
             L"Effective render request: 44100 Hz / 1 ch / PCM16";
}

bool TestBuildRunningDeviceSelectionDriftSummaryText() {
  return BuildRunningDeviceSelectionDriftSummaryText(
             L"Idle", false, L"cap-b", L"ren-b", L"cap-a", L"ren-a")
             .empty() &&
         BuildRunningDeviceSelectionDriftSummaryText(
             L"Running", false, L"cap-b", L"ren-a", L"cap-a", L"ren-a") ==
             L"Running session note: the current capture device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous capture device." &&
         BuildRunningDeviceSelectionDriftSummaryText(
             L"Running", false, L"cap-a", L"ren-b", L"cap-a", L"ren-a") ==
             L"Running session note: the current render device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous render device." &&
         BuildRunningDeviceSelectionDriftSummaryText(
             L"Running", true, L"cap-b", L"ren-b", L"cap-a", L"ren-a")
             .empty() &&
         BuildRunningDeviceSelectionDriftSummaryText(
             L"Running", false, L"cap-b", L"ren-b", L"cap-a", L"ren-a") ==
             L"Running session note: current capture/render device picks apply to the next rebuilt or restarted session, while the already-active stream still uses the previous devices.";
}

bool TestBuildRunningDeviceChangeSummaryText() {
  return BuildRunningDeviceChangeSummaryText(L"Idle", L"default-device-change", L"tracked-no-rebuild").empty() &&
         BuildRunningDeviceChangeSummaryText(L"Running", L"default-device-change", L"tracked-no-rebuild") ==
             L"Running session note: the default-device-change was tracked, but the already-active session kept its current devices because follow-defaults is off." &&
         BuildRunningDeviceChangeSummaryText(L"Running", L"refresh-devices", L"rebuild-success") ==
             L"Running session note: the active session was rebuilt successfully after the device change.";
}

bool TestBuildCaptureDeviceLabelText() {
  return BuildCaptureDeviceLabelText(false) == L"Capture Device" &&
         BuildCaptureDeviceLabelText(true) == L"Loopback Capture Device" &&
         BuildCaptureDeviceLabelText(AudioSourceMode::ApplicationProcessLoopback) ==
             L"App Loopback Source" &&
         BuildCaptureDeviceLabelText(AudioSourceMode::ApplicationLoopback) ==
             L"App Loopback Source";
}

bool TestBuildDeviceCountLineText() {
  return BuildDeviceCountLineText(false, 2, 3) ==
             L"Capture devices: 2 | Render devices: 3" &&
         BuildDeviceCountLineText(true, 2, 3) ==
             L"Loopback capture devices: 2 | Render devices: 3" &&
         BuildDeviceCountLineText(AudioSourceMode::ApplicationProcessLoopback, 1, 3) ==
             L"Application loopback sources: 1 | Render devices: 3" &&
         BuildDeviceCountLineText(AudioSourceMode::ApplicationLoopback, 1, 3) ==
             L"Application loopback sources: 1 | Render devices: 3";
}

bool TestBuildLoopbackCaptureNoteText() {
  return BuildLoopbackCaptureNoteText(false).empty() &&
         BuildLoopbackCaptureNoteText(true) ==
             L"Loopback capture uses render endpoints as capture sources." &&
         BuildLoopbackCaptureNoteText(AudioSourceMode::ApplicationProcessLoopback) ==
             L"Application process loopback captures audio rendered by a target process tree instead of a device endpoint." &&
         BuildLoopbackCaptureNoteText(AudioSourceMode::ApplicationLoopback) ==
             L"Application loopback captures audio rendered by a target process tree instead of a device endpoint.";
}

bool TestBuildLoopbackBackendNoteText() {
  return BuildLoopbackBackendNoteText(false, true).empty() &&
         BuildLoopbackBackendNoteText(true, true) ==
             L"Loopback note: WASAPI loopback is shared-mode only" &&
         BuildLoopbackBackendNoteText(true, false) ==
             L"Loopback note: WAVE loopback depends on hardware/driver devices" &&
         BuildLoopbackBackendNoteText(AudioSourceMode::ApplicationProcessLoopback, true) ==
             L"Application loopback note: process-tree loopback requires a WASAPI capture path and newer Windows support." &&
         BuildLoopbackBackendNoteText(AudioSourceMode::ApplicationLoopback, true) ==
             L"Application loopback note: process-tree loopback requires a WASAPI capture path and newer Windows support.";
}

bool TestBuildFollowDefaultsNoteText() {
  return BuildFollowDefaultsNoteText(false, false).empty() &&
         BuildFollowDefaultsNoteText(true, false) ==
             L"Device selection follows current system defaults; manual device picks are inactive" &&
         BuildFollowDefaultsNoteText(true, true) ==
             L"Device selection follows current system defaults; manual device picks are inactive and loopback capture follows the current default render endpoint" &&
         BuildFollowDefaultsNoteText(true, AudioSourceMode::ApplicationProcessLoopback) ==
             L"Device selection follows current system defaults for render monitoring; app-loopback capture itself is process-targeted." &&
         BuildFollowDefaultsNoteText(true, AudioSourceMode::ApplicationLoopback) ==
             L"Device selection follows current system defaults for render monitoring; app-loopback capture itself is process-targeted.";
}

bool TestBuildFollowDefaultsDiagnosticsText() {
  return BuildFollowDefaultsDiagnosticsText(false, false).empty() &&
         BuildFollowDefaultsDiagnosticsText(true, false) ==
             L"Device tracking: current capture/render selection follows system defaults" &&
         BuildFollowDefaultsDiagnosticsText(true, true) ==
             L"Device tracking: current capture/render selection follows system defaults, and loopback capture follows the current default render endpoint" &&
         BuildFollowDefaultsDiagnosticsText(true, AudioSourceMode::ApplicationProcessLoopback) ==
             L"Device tracking: render monitoring follows system defaults while application loopback capture remains process-targeted" &&
         BuildFollowDefaultsDiagnosticsText(true, AudioSourceMode::ApplicationLoopback) ==
             L"Device tracking: render monitoring follows system defaults while application loopback capture remains process-targeted";
}

bool TestBuildApplicationLoopbackText() {
  return BuildApplicationLoopbackTargetSummaryText(
                 ApplicationLoopbackTargetKind::ApplicationName, L"spotify.exe") ==
             L"App loopback application: spotify.exe" &&
         BuildApplicationLoopbackTargetSummaryText(
                 ApplicationLoopbackTargetKind::ApplicationName, L"") ==
             L"App loopback application: not set" &&
         BuildApplicationLoopbackTargetSummaryText(
                 ApplicationLoopbackTargetKind::ProcessId, L"1234") ==
             L"App loopback process id: 1234" &&
         BuildApplicationLoopbackNoteText(
                 ApplicationLoopbackTargetKind::ApplicationName, L"") ==
             L"Application loopback needs a target application name." &&
         BuildApplicationLoopbackNoteText(
                 ApplicationLoopbackTargetKind::ProcessId, L"") ==
             L"Application loopback needs a target process id." &&
         BuildApplicationLoopbackDiagnosticsText(
                 ApplicationLoopbackTargetKind::ProcessId, L"1234") ==
             L"Application loopback target process id: 1234";
}

bool TestBuildMonitorDisabledNoteText() {
  return BuildMonitorDisabledNoteText(true, true, L"Idle").empty() &&
         BuildMonitorDisabledNoteText(false, false, L"Idle") ==
             L"Render pipeline disabled for monitoring; render-only settings are inactive" &&
         BuildMonitorDisabledNoteText(false, true, L"Running") ==
             L"Render monitor playback is turned off for the next rebuilt or restarted session; the already-active render stream is still running.";
}

bool TestBuildMonitorDisabledDiagnosticsText() {
  return BuildMonitorDisabledDiagnosticsText(true, true, L"Idle").empty() &&
         BuildMonitorDisabledDiagnosticsText(false, false, L"Idle") ==
             L"Render pipeline disabled: render-only settings are inactive while monitor playback is off" &&
         BuildMonitorDisabledDiagnosticsText(false, true, L"Running") ==
             L"Render monitor playback is disabled in the current configuration, but the already-active session still has render monitoring until the next rebuild or restart.";
}

bool TestBuildMonitorDisabledRenderWaveNoteText() {
  return BuildMonitorDisabledRenderWaveNoteText(true).empty() &&
         BuildMonitorDisabledRenderWaveNoteText(false) ==
             L"render monitoring is disabled because monitor playback is off";
}

bool TestBuildLastDeviceChangeDiagnosticsText() {
  return BuildLastDeviceChangeDiagnosticsText(L"", L"tracked-no-rebuild").empty() &&
         BuildLastDeviceChangeDiagnosticsText(L"default-device-change",
                                              L"tracked-no-rebuild") ==
             L"Last device change: default-device-change => tracked-no-rebuild";
}

bool TestBuildLastRebuildDiagnosticsText() {
  return BuildLastRebuildDiagnosticsText(L"", L"success").empty() &&
         BuildLastRebuildDiagnosticsText(L"default-device-change", L"success") ==
             L"Last rebuild: default-device-change => success";
}

bool TestBuildRunningSessionConfigurationNoteText() {
  return BuildRunningSessionConfigurationNoteText(L"Idle").empty() &&
         BuildRunningSessionConfigurationNoteText(L"Running") ==
             L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.";
}

bool TestBuildActiveDiagnosticsLabels() {
  return BuildCurrentConfiguredCaptureDiagnosticsLabelText() == L"Current configured capture: " &&
         BuildCurrentConfiguredRenderDiagnosticsLabelText() == L"Current configured render: " &&
         BuildEffectiveConfiguredRenderRequestDiagnosticsLabelText() == L"Effective configured render request: " &&
         BuildActiveRequestedCaptureDiagnosticsLabelText() == L"Active session requested capture: " &&
         BuildActiveRequestedRenderDiagnosticsLabelText() == L"Active session requested render: " &&
         BuildActiveRequestedCaptureDeviceIdDiagnosticsLabelText() == L"Active session requested capture id: " &&
         BuildActiveRequestedRenderDeviceIdDiagnosticsLabelText() == L"Active session requested render id: " &&
         BuildActiveNegotiatedCaptureDiagnosticsLabelText() == L"Active session negotiated capture: " &&
         BuildActiveNegotiatedRenderDiagnosticsLabelText() == L"Active session negotiated render: " &&
         BuildActiveCaptureModeDiagnosticsLabelText() == L"Active capture mode: " &&
         BuildActiveRenderModeDiagnosticsLabelText() == L"Active render mode: " &&
         BuildActiveResamplerDiagnosticsLabelText() == L"Active resampler: " &&
         BuildActiveCaptureRuntimeDiagnosticsLabelText() == L"Active capture runtime: " &&
         BuildActiveRenderRuntimeDiagnosticsLabelText() == L"Active render runtime: " &&
         BuildActiveCaptureWasapiRequestDiagnosticsLabelText() == L"Active capture WASAPI request: " &&
         BuildActiveRenderWasapiRequestDiagnosticsLabelText() == L"Active render WASAPI request: " &&
         BuildActiveMonitorDelayDiagnosticsLabelText() == L"Active monitor delay: " &&
         BuildActiveCaptureBufferDiagnosticsLabelText() == L"Active capture buffer: " &&
         BuildActiveRenderBufferDiagnosticsLabelText() == L"Active render buffer: ";
}

bool TestBuildSelectedCaptureDeviceDiagnosticsLabelText() {
  return BuildSelectedCaptureDeviceDiagnosticsLabelText(false) ==
             L"Selected capture device: " &&
         BuildSelectedCaptureDeviceDiagnosticsLabelText(true) ==
             L"Selected loopback capture device: " &&
         BuildSelectedCaptureDeviceDiagnosticsLabelText(
             AudioSourceMode::ApplicationProcessLoopback) ==
             L"Selected app-loopback source: " &&
         BuildSelectedCaptureDeviceDiagnosticsLabelText(
             AudioSourceMode::ApplicationLoopback) ==
             L"Selected app-loopback source: ";
}

bool TestBuildSelectedCaptureDeviceIdDiagnosticsLabelText() {
  return BuildSelectedCaptureDeviceIdDiagnosticsLabelText(false) ==
             L"Selected capture id: " &&
         BuildSelectedCaptureDeviceIdDiagnosticsLabelText(true) ==
             L"Selected loopback capture id: " &&
         BuildSelectedCaptureDeviceIdDiagnosticsLabelText(
             AudioSourceMode::ApplicationProcessLoopback) ==
             L"Selected app-loopback id: " &&
         BuildSelectedCaptureDeviceIdDiagnosticsLabelText(
             AudioSourceMode::ApplicationLoopback) ==
             L"Selected app-loopback id: ";
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    bool (*fn)();
  };

  const std::vector<NamedTest> tests = {
      {"BuildWindowTitleIdle", &TestBuildWindowTitleIdle},
      {"BuildWindowTitleFormatText", &TestBuildWindowTitleFormatText},
      {"BuildWindowTitleQuickBusy", &TestBuildWindowTitleQuickBusy},
      {"BuildWindowTitleMatrixBusy", &TestBuildWindowTitleMatrixBusy},
      {"BuildProbeButtonLabel", &TestBuildProbeButtonLabel},
      {"BuildProbeMatrixButtonLabel", &TestBuildProbeMatrixButtonLabel},
      {"BuildAutoAlignExplanatoryNoteText",
       &TestBuildAutoAlignExplanatoryNoteText},
      {"BuildEffectiveRenderRequestSummaryText",
       &TestBuildEffectiveRenderRequestSummaryText},
      {"BuildRunningDeviceSelectionDriftSummaryText",
       &TestBuildRunningDeviceSelectionDriftSummaryText},
      {"BuildRunningDeviceChangeSummaryText",
       &TestBuildRunningDeviceChangeSummaryText},
      {"BuildCaptureDeviceLabelText", &TestBuildCaptureDeviceLabelText},
      {"BuildDeviceCountLineText", &TestBuildDeviceCountLineText},
      {"BuildLoopbackCaptureNoteText", &TestBuildLoopbackCaptureNoteText},
      {"BuildLoopbackBackendNoteText", &TestBuildLoopbackBackendNoteText},
      {"BuildFollowDefaultsNoteText", &TestBuildFollowDefaultsNoteText},
      {"BuildFollowDefaultsDiagnosticsText",
       &TestBuildFollowDefaultsDiagnosticsText},
      {"BuildApplicationLoopbackText", &TestBuildApplicationLoopbackText},
      {"BuildMonitorDisabledNoteText", &TestBuildMonitorDisabledNoteText},
      {"BuildMonitorDisabledDiagnosticsText",
       &TestBuildMonitorDisabledDiagnosticsText},
      {"BuildMonitorDisabledRenderWaveNoteText",
       &TestBuildMonitorDisabledRenderWaveNoteText},
      {"BuildLastDeviceChangeDiagnosticsText",
       &TestBuildLastDeviceChangeDiagnosticsText},
      {"BuildLastRebuildDiagnosticsText",
       &TestBuildLastRebuildDiagnosticsText},
      {"BuildRunningSessionConfigurationNoteText",
       &TestBuildRunningSessionConfigurationNoteText},
      {"BuildActiveDiagnosticsLabels",
       &TestBuildActiveDiagnosticsLabels},
      {"BuildSelectedCaptureDeviceDiagnosticsLabelText",
       &TestBuildSelectedCaptureDeviceDiagnosticsLabelText},
      {"BuildSelectedCaptureDeviceIdDiagnosticsLabelText",
       &TestBuildSelectedCaptureDeviceIdDiagnosticsLabelText},
  };

  for (const auto& test : tests) {
    std::cerr << "RUNNING: " << test.name << "\n";
    if (!test.fn()) {
      std::cerr << "FAILED: " << test.name << "\n";
      return 1;
    }
  }

  std::cerr << "ALL_TESTS_PASSED\n";
  return 0;
}
