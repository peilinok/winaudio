#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "app/app_model.h"
#include "audio/backends/stub_backends.h"

using namespace winaudio;

namespace {

AppModel MakeStubBackedModel() {
  return AppModel(std::make_unique<StubAudioBackendFactory>());
}

bool Contains(const std::wstring& text, const std::wstring& needle) {
  return text.find(needle) != std::wstring::npos;
}

size_t CountOccurrences(const std::wstring& text, const std::wstring& needle) {
  if (needle.empty()) {
    return 0;
  }

  size_t count = 0;
  size_t position = 0;
  while ((position = text.find(needle, position)) != std::wstring::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

bool TestSummaryTextReflectsConfiguration() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSampleRate(44100);
  model.SetCaptureChannels(1);
  model.SetCaptureSampleType(AudioSampleType::PcmInt24);
  model.SetRenderSampleRate(48000);
  model.SetRenderChannels(2);
  model.SetRenderSampleType(AudioSampleType::PcmInt32);
  model.SetFixedDelayMs(250);
  model.SetDumpEnabled(true);
  model.SetDumpPath(L"C:\\temp\\capture.wav");
  model.SetDumpFileType(DumpFileType::Wav);
  model.SetCaptureWasapiShareMode(WasapiShareMode::Exclusive);
  model.SetCaptureWasapiDriveMode(WasapiDriveMode::TimerDriven);
  model.SetRenderWasapiShareMode(WasapiShareMode::Shared);
  model.SetRenderWasapiDriveMode(WasapiDriveMode::EventDriven);
  model.SetCaptureBufferDurationMs(60);
  model.SetRenderBufferDurationMs(80);
  model.SetMonitorEnabled(false);
  model.SetFollowDefaultDevices(true);
  model.SetAutoAlignRenderFormat(false);

  const auto summary = model.summary_text();
  return Contains(summary, L"Capture: WASAPI / Microphone / 44100 Hz / 1 ch / PCM24") &&
         Contains(summary, L"Render: WASAPI / 48000 Hz / 2 ch / PCM32") &&
         Contains(summary, L"Monitor delay: 250 ms") &&
         Contains(summary, L"Monitor: Off") &&
         Contains(summary, L"Follow defaults: On") &&
         Contains(summary, L"Device selection follows current system defaults; manual device picks are inactive") &&
         Contains(summary, L"Render auto-align: Off") &&
         Contains(summary, L"Capture WASAPI: Exclusive / Timer") &&
         Contains(summary, L"Render WASAPI: Shared / Event") &&
         Contains(summary, L"Dump: On (WAV)") &&
         Contains(summary, L"Dump path: C:\\temp\\capture.wav") &&
         Contains(summary, L"Capture buffer: 60 ms") &&
         Contains(summary, L"Render buffer: 80 ms") &&
         Contains(summary, L"Capture: WASAPI / Microphone / 44100 Hz / 1 ch / PCM24");
}

bool TestSummaryExplainsFollowDefaultsLoopbackBinding() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);
  model.SetFollowDefaultDevices(true);

  const auto summary = model.summary_text();
  return Contains(summary,
                  L"Device selection follows current system defaults; manual device picks are inactive and loopback capture follows the current default render endpoint");
}

bool TestSummaryShowsApplicationLoopbackTargetAndNote() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::ApplicationLoopback);
  model.SetApplicationLoopbackProcess(L"spotify.exe");

  const auto summary = model.summary_text();
  return Contains(summary,
                  L"Capture: WASAPI / Application Loopback / 48000 Hz / 2 ch / Float32") &&
         Contains(summary, L"App loopback target: spotify.exe") &&
         Contains(summary,
                  L"Application loopback captures audio rendered by a target process tree instead of a device endpoint.") &&
         Contains(summary,
                  L"Application loopback is unavailable on this machine because Windows process loopback requires client build 20348 or newer.");
}

bool TestDiagnosticsExplainApplicationLoopbackTarget() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::ApplicationLoopback);
  model.SetApplicationLoopbackProcess(L"1234");

  const auto diagnostics = model.diagnostics_text();
  return Contains(diagnostics,
                  L"Application loopback target process: 1234");
}

bool TestCapabilityTextExplainsApplicationLoopbackLimitation() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::ApplicationLoopback);

  const auto capability = model.capability_text();
  return Contains(capability,
                  L"Application loopback is unavailable on this machine because Windows process loopback requires client build 20348 or newer");
}

bool TestSummaryShowsApplicationLoopbackTargetMissingNote() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::ApplicationLoopback);

  const auto summary = model.summary_text();
  return Contains(summary, L"App loopback target: not set") &&
         Contains(summary,
                  L"Application loopback needs a target process name or PID.");
}

bool TestSummaryShowsEffectiveRenderRequestWhenAutoAlignEnabled() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSampleRate(44100);
  model.SetCaptureChannels(1);
  model.SetCaptureSampleType(AudioSampleType::PcmInt16);
  model.SetRenderSampleRate(48000);
  model.SetRenderChannels(2);
  model.SetRenderSampleType(AudioSampleType::Float32);
  model.SetAutoAlignRenderFormat(true);

  const auto summary = model.summary_text();
  return Contains(summary, L"Render auto-align: On") &&
         Contains(summary, L"Effective render request: 44100 Hz / 1 ch / PCM16");
}

bool TestRunningSessionSummaryShowsConfigurationNote() {
  AppModel model = MakeStubBackedModel();
  model.OnSessionStateChanged(L"Running");
  const auto summary = model.summary_text();
  return Contains(summary,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestDiagnosticsTextIncludesRuntimeAndSelectedDevices() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"cap-dev");
  model.SetRenderDeviceId(L"ren-dev");

  DeviceEnumerationSnapshot snapshot;
  snapshot.capture_devices.push_back(
      {L"cap-dev", L"Mic", true, AudioDirection::Capture, false, 0});
  snapshot.render_devices.push_back(
      {L"ren-dev", L"Speaker", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(snapshot);

  SessionRuntimeStats stats;
  stats.requested_capture_format = L"44100 Hz / 1 ch / PCM16";
  stats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  stats.requested_capture_device_id = L"default";
  stats.requested_render_device_id = L"default";
  stats.negotiated_capture_format = L"48000 Hz / 2 ch / PCM16";
  stats.negotiated_render_format = L"44100 Hz / 2 ch / PCM16";
  stats.actual_capture_backend_mode = L"WASAPI Shared / Event";
  stats.actual_render_backend_mode = L"WAVE API Callback";
  stats.actual_resampler_mode = L"MediaFoundation";
  stats.capture_runtime_details = L"Capture runtime details";
  stats.render_runtime_details = L"Render runtime details";
  stats.active_requested_wasapi_mode_present = true;
  stats.requested_capture_wasapi_mode = L"Shared / Event";
  stats.requested_render_wasapi_mode = L"Exclusive / Timer";
  stats.active_requested_timing_present = true;
  stats.requested_monitor_delay_ms = 120;
  stats.requested_capture_buffer_duration_ms = 40;
  stats.requested_render_buffer_duration_ms = 40;
  stats.dump_path = L"C:\\temp\\active.wav";
  stats.last_device_change_reason = L"default-device-change";
  stats.last_device_change_result = L"rebuild-success";
  stats.last_rebuild_reason = L"default-device-change";
  stats.last_rebuild_result = L"success";
  stats.last_error_stage = L"render-start";
  stats.last_error_message = L"Failed to start render adapter.";
  model.OnStatsUpdated(stats);

  const auto diagnostics = model.diagnostics_text();
  return Contains(diagnostics, L"Current configured capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Current configured render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Effective configured render request: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested capture: 44100 Hz / 1 ch / PCM16") &&
         Contains(diagnostics, L"Active session requested render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested capture id: default") &&
         Contains(diagnostics, L"Active session requested render id: default") &&
         Contains(diagnostics, L"Active session negotiated capture: 48000 Hz / 2 ch / PCM16") &&
         Contains(diagnostics, L"Active session negotiated render: 44100 Hz / 2 ch / PCM16") &&
         Contains(diagnostics, L"Active capture mode: WASAPI Shared / Event") &&
         Contains(diagnostics, L"Active render mode: WAVE API Callback") &&
         Contains(diagnostics, L"Active resampler: MediaFoundation") &&
         Contains(diagnostics, L"Active capture runtime: Capture runtime details") &&
         Contains(diagnostics, L"Active render runtime: Render runtime details") &&
         Contains(diagnostics, L"Active capture WASAPI request: Shared / Event") &&
         Contains(diagnostics, L"Active render WASAPI request: Exclusive / Timer") &&
         Contains(diagnostics, L"Active monitor delay: 120 ms") &&
         Contains(diagnostics, L"Active capture buffer: 40 ms") &&
         Contains(diagnostics, L"Active render buffer: 40 ms") &&
         Contains(diagnostics, L"Active dump path: C:\\temp\\active.wav") &&
         Contains(diagnostics, L"Last device change: default-device-change => rebuild-success") &&
         Contains(diagnostics, L"Last rebuild: default-device-change => success") &&
         Contains(diagnostics, L"Last error: render-start => Failed to start render adapter.") &&
         Contains(diagnostics, L"Selected capture device: Mic | Capture | Default") &&
         Contains(diagnostics, L"Selected capture id: cap-dev") &&
         Contains(diagnostics, L"Selected render device: Speaker | Render | Default | Loopback") &&
         Contains(diagnostics, L"Selected render id: ren-dev");
}

bool TestDiagnosticsSuppressStaleLastRebuildAfterTrackedNoRebuildChange() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats stats;
  stats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  stats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  stats.last_device_change_reason = L"default-device-change";
  stats.last_device_change_result = L"tracked-no-rebuild";
  stats.last_rebuild_reason = L"refresh-devices";
  stats.last_rebuild_result = L"success";
  model.OnStatsUpdated(stats);

  const auto diagnostics = model.diagnostics_text();
  return Contains(diagnostics, L"Last device change: default-device-change => tracked-no-rebuild") &&
         !Contains(diagnostics, L"Last rebuild:");
}

bool TestDefaultDeviceSelectionBackfillsEmptyIds() {
  AppModel model = MakeStubBackedModel();

  DeviceEnumerationSnapshot snapshot;
  snapshot.capture_devices.push_back(
      {L"default-cap", L"Default Mic", true, AudioDirection::Capture, false, 0});
  snapshot.render_devices.push_back(
      {L"default-ren", L"Default Speaker", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(snapshot);

  const auto config = model.configuration();
  const auto diagnostics = model.diagnostics_text();
  return config.capture.device_id == L"default-cap" &&
         config.render.device_id == L"default-ren" &&
         Contains(diagnostics, L"Selected capture device: Default Mic | Capture | Default") &&
         Contains(diagnostics, L"Selected render device: Default Speaker | Render | Default | Loopback");
}

bool TestDevicesUpdatedReplacesStaleCaptureIdWithCurrentDefault() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"stale-capture-id");

  DeviceEnumerationSnapshot snapshot;
  snapshot.capture_devices.push_back(
      {L"default-cap", L"Default Mic", true, AudioDirection::Capture, false, 0});
  snapshot.render_devices.push_back(
      {L"default-ren", L"Default Speaker", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(snapshot);

  const auto config = model.configuration();
  const auto diagnostics = model.diagnostics_text();
  return config.capture.device_id == L"default-cap" &&
         Contains(diagnostics, L"Selected capture id: default-cap");
}

bool TestDevicesUpdatedReplacesStaleRenderIdWithCurrentDefault() {
  AppModel model = MakeStubBackedModel();
  model.SetRenderDeviceId(L"stale-render-id");

  DeviceEnumerationSnapshot snapshot;
  snapshot.capture_devices.push_back(
      {L"default-cap", L"Default Mic", true, AudioDirection::Capture, false, 0});
  snapshot.render_devices.push_back(
      {L"default-ren", L"Default Speaker", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(snapshot);

  const auto config = model.configuration();
  const auto diagnostics = model.diagnostics_text();
  return config.render.device_id == L"default-ren" &&
         Contains(diagnostics, L"Selected render id: default-ren");
}

bool TestRefreshDevicesStoresReturnedSnapshotWithoutActiveSink() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureBackend(AudioBackendType::WaveApi);
  model.SetRenderBackend(AudioBackendType::WaveApi);
  model.SetCaptureSourceMode(AudioSourceMode::MicrophoneCapture);
  model.RefreshDevices();

  const auto devices = model.devices();
  return !devices.capture_devices.empty() && !devices.render_devices.empty();
}

bool TestRefreshDevicesPreservesRunningSessionState() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.RefreshDevices();

  const auto state = model.session_state();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return state == L"Running" &&
         Contains(diagnostics, L"Current configured capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Current configured render: 48000 Hz / 2 ch / Float32");
}

bool TestSourceModeChangePreservesRunningSessionState() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);

  const auto state = model.session_state();
  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return state == L"Running" &&
         Contains(summary, L"Capture: WASAPI / System Loopback / 48000 Hz / 2 ch / Float32") &&
         Contains(summary, L"Loopback capture uses render endpoints as capture sources.") &&
         Contains(diagnostics, L"Current configured capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestSourceModeChangeShowsSummaryDriftNoteWhileRunning() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"cap-b");

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.requested_render_device_id = L"ren-a";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);

  const auto summary = model.summary_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
      summary,
      L"Running session note: current capture/render device picks apply to the next rebuilt or restarted session");
}

bool TestRenderBackendChangePreservesRunningSessionState() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.actual_render_backend_mode = L"WASAPI Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetRenderBackend(AudioBackendType::WaveApi);

  const auto state = model.session_state();
  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  const auto config = model.configuration();
  model.OnSessionStateChanged(L"Stopped");

  const bool selected_render_id_changed =
      config.render.device_id != L"";
  return state == L"Running" &&
         config.render.backend == AudioBackendType::WaveApi &&
         selected_render_id_changed &&
         Contains(summary, L"Render: WAVE API / 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Current configured render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active render mode: WASAPI Shared / Event") &&
         Contains(diagnostics, L"Selected render id: ") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestRenderBackendChangeShowsSummaryDriftNoteWhileRunning() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats runningStats;
  runningStats.requested_render_device_id = L"ren-a";
  runningStats.actual_render_backend_mode = L"WASAPI Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetRenderBackend(AudioBackendType::WaveApi);

  const auto summary = model.summary_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
      summary,
      L"Running session note: the current render device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous render device.");
}

bool TestCaptureBackendChangePreservesRunningSessionState() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.actual_capture_backend_mode = L"WASAPI Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureBackend(AudioBackendType::WaveApi);

  const auto state = model.session_state();
  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  const auto config = model.configuration();
  model.OnSessionStateChanged(L"Stopped");

  const bool selected_capture_id_changed =
      config.capture.device_id != L"";
  return state == L"Running" &&
         config.capture.backend == AudioBackendType::WaveApi &&
         selected_capture_id_changed &&
         Contains(summary, L"Capture: WAVE API / Microphone / 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Current configured capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active capture mode: WASAPI Shared / Event") &&
         Contains(diagnostics, L"Selected capture id: ") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestCaptureBackendChangeShowsSummaryDriftNoteWhileRunning() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.actual_capture_backend_mode = L"WASAPI Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureBackend(AudioBackendType::WaveApi);

  const auto summary = model.summary_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
      summary,
      L"Running session note: the current capture device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous capture device.");
}

bool TestFollowDefaultsWhileRunningShowsSummaryTrackingNote() {
  AppModel model = MakeStubBackedModel();
  model.OnSessionStateChanged(L"Running");
  model.SetFollowDefaultDevices(true);

  const auto summary = model.summary_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
      summary,
      L"Device selection follows current system defaults; manual device picks are inactive");
}

bool TestFollowDefaultsSourceModeChangeShowsRunningLoopbackTrackingNote() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.requested_render_device_id = L"ren-a";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");
  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
             summary,
             L"Device selection follows current system defaults; manual device picks are inactive and loopback capture follows the current default render endpoint") &&
         !Contains(
             summary,
             L"Running session note: the current capture device pick applies to the next rebuilt or restarted session, while the already-active stream still uses the previous capture device.") &&
         Contains(
             diagnostics,
             L"Device tracking: current capture/render selection follows system defaults, and loopback capture follows the current default render endpoint");
}

bool TestFollowDefaultsCaptureBackendChangeShowsRunningTrackingNote() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.requested_render_device_id = L"ren-a";
  runningStats.actual_capture_backend_mode = L"WASAPI Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");
  model.SetCaptureBackend(AudioBackendType::WaveApi);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
             summary,
             L"Device selection follows current system defaults; manual device picks are inactive") &&
         !Contains(
             summary,
             L"Running session note: the current capture device pick applies to the next rebuilt or restarted session") &&
         Contains(
             diagnostics,
             L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(diagnostics, L"Active capture mode: WASAPI Shared / Event");
}

bool TestFollowDefaultsRenderBackendChangeShowsRunningTrackingNote() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.requested_render_device_id = L"ren-a";
  runningStats.actual_render_backend_mode = L"WASAPI Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");
  model.SetRenderBackend(AudioBackendType::WaveApi);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
             summary,
             L"Device selection follows current system defaults; manual device picks are inactive") &&
         !Contains(
             summary,
             L"Running session note: the current render device pick applies to the next rebuilt or restarted session") &&
         Contains(
             diagnostics,
             L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(diagnostics, L"Active render mode: WASAPI Shared / Event");
}

bool TestFollowDefaultsMonitorOffWhileRunningShowsCombinedSummaryAndDiagnostics() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.requested_render_device_id = L"ren-a";
  runningStats.active_render_monitor_enabled = true;
  runningStats.actual_render_backend_mode = L"WASAPI Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");
  model.SetMonitorEnabled(false);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
             summary,
             L"Device selection follows current system defaults; manual device picks are inactive") &&
         Contains(
             summary,
             L"Render monitor playback is turned off for the next rebuilt or restarted session; the already-active render stream is still running.") &&
         Contains(
             diagnostics,
             L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(
             diagnostics,
             L"Render monitor playback is disabled in the current configuration, but the already-active session still has render monitoring until the next rebuild or restart.");
}

bool TestManualDeviceIdChangePreservesActiveRequestedDeviceIdsWhileRunning() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"cap-a");
  model.SetRenderDeviceId(L"ren-a");

  DeviceEnumerationSnapshot snapshot;
  snapshot.capture_devices.push_back(
      {L"cap-a", L"Mic A", true, AudioDirection::Capture, false, 0});
  snapshot.capture_devices.push_back(
      {L"cap-b", L"Mic B", false, AudioDirection::Capture, false, 0});
  snapshot.render_devices.push_back(
      {L"ren-a", L"Speaker A", true, AudioDirection::Render, true, 0});
  snapshot.render_devices.push_back(
      {L"ren-b", L"Speaker B", false, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(snapshot);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.requested_render_device_id = L"ren-a";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureDeviceId(L"cap-b");
  model.SetRenderDeviceId(L"ren-b");

  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(diagnostics, L"Active session requested capture id: cap-a") &&
         Contains(diagnostics, L"Active session requested render id: ren-a") &&
         Contains(diagnostics, L"Selected capture id: cap-b") &&
         Contains(diagnostics, L"Selected render id: ren-b") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestManualDeviceIdChangeShowsSummaryDriftNoteWhileRunning() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"cap-a");
  model.SetRenderDeviceId(L"ren-a");

  DeviceEnumerationSnapshot snapshot;
  snapshot.capture_devices.push_back(
      {L"cap-a", L"Mic A", true, AudioDirection::Capture, false, 0});
  snapshot.capture_devices.push_back(
      {L"cap-b", L"Mic B", false, AudioDirection::Capture, false, 0});
  snapshot.render_devices.push_back(
      {L"ren-a", L"Speaker A", true, AudioDirection::Render, true, 0});
  snapshot.render_devices.push_back(
      {L"ren-b", L"Speaker B", false, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(snapshot);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.requested_render_device_id = L"ren-a";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureDeviceId(L"cap-b");
  model.SetRenderDeviceId(L"ren-b");

  const auto summary = model.summary_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
      summary,
      L"Running session note: current capture/render device picks apply to the next rebuilt or restarted session, while the already-active stream still uses the previous devices.");
}

bool TestManualDeviceIdsPersistAcrossDevicesUpdatedWhileRunning() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"cap-a");
  model.SetRenderDeviceId(L"ren-a");

  DeviceEnumerationSnapshot first;
  first.capture_devices.push_back(
      {L"cap-a", L"Mic A", true, AudioDirection::Capture, false, 0});
  first.capture_devices.push_back(
      {L"cap-b", L"Mic B", false, AudioDirection::Capture, false, 0});
  first.render_devices.push_back(
      {L"ren-a", L"Speaker A", true, AudioDirection::Render, true, 0});
  first.render_devices.push_back(
      {L"ren-b", L"Speaker B", false, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(first);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_device_id = L"cap-a";
  runningStats.requested_render_device_id = L"ren-a";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureDeviceId(L"cap-b");
  model.SetRenderDeviceId(L"ren-b");

  DeviceEnumerationSnapshot second;
  second.capture_devices.push_back(
      {L"cap-a", L"Mic A", true, AudioDirection::Capture, false, 0});
  second.capture_devices.push_back(
      {L"cap-b", L"Mic B", false, AudioDirection::Capture, false, 0});
  second.render_devices.push_back(
      {L"ren-a", L"Speaker A", true, AudioDirection::Render, true, 0});
  second.render_devices.push_back(
      {L"ren-b", L"Speaker B", false, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(second);

  const auto diagnostics = model.diagnostics_text();
  const auto config = model.configuration();
  model.OnSessionStateChanged(L"Stopped");

  return config.capture.device_id == L"cap-b" &&
         config.render.device_id == L"ren-b" &&
         Contains(diagnostics, L"Active session requested capture id: cap-a") &&
         Contains(diagnostics, L"Active session requested render id: ren-a") &&
         Contains(diagnostics, L"Selected capture id: cap-b") &&
         Contains(diagnostics, L"Selected render id: ren-b");
}

bool TestRunningDumpConfigEditPreservesActiveDumpPathSemantics() {
  AppModel model = MakeStubBackedModel();
  model.SetDumpEnabled(false);
  model.SetDumpPath(L"");

  SessionRuntimeStats runningStats;
  runningStats.dump_path = L"C:\\temp\\active.wav";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetDumpEnabled(true);
  model.SetDumpPath(L"C:\\temp\\next.wav");

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(summary, L"Dump: On (WAV)") &&
         Contains(summary, L"Dump path: C:\\temp\\next.wav") &&
         Contains(diagnostics, L"Active dump path: C:\\temp\\active.wav") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestFollowDefaultsRunningDumpConfigEditPreservesTrackingAndActiveDumpPath() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);
  model.SetDumpEnabled(false);
  model.SetDumpPath(L"");

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.dump_path = L"C:\\temp\\active.wav";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetDumpEnabled(true);
  model.SetDumpPath(L"C:\\temp\\next.wav");

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(summary, L"Follow defaults: On") &&
         Contains(summary, L"Dump: On (WAV)") &&
         Contains(summary, L"Dump path: C:\\temp\\next.wav") &&
         Contains(diagnostics, L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(diagnostics, L"Active dump path: C:\\temp\\active.wav") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestRunningTimingConfigEditPreservesActiveTimingDiagnostics() {
  AppModel model = MakeStubBackedModel();
  model.SetFixedDelayMs(120);
  model.SetCaptureBufferDurationMs(40);
  model.SetRenderBufferDurationMs(40);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.active_requested_timing_present = true;
  runningStats.requested_monitor_delay_ms = 120;
  runningStats.requested_capture_buffer_duration_ms = 40;
  runningStats.requested_render_buffer_duration_ms = 40;
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetFixedDelayMs(250);
  model.SetCaptureBufferDurationMs(60);
  model.SetRenderBufferDurationMs(80);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(summary, L"Monitor delay: 250 ms") &&
         Contains(summary, L"Capture buffer: 60 ms") &&
         Contains(summary, L"Render buffer: 80 ms") &&
         Contains(diagnostics, L"Active monitor delay: 120 ms") &&
         Contains(diagnostics, L"Active capture buffer: 40 ms") &&
         Contains(diagnostics, L"Active render buffer: 40 ms") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestFollowDefaultsRunningTimingConfigEditPreservesTrackingAndActiveTiming() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);
  model.SetFixedDelayMs(120);
  model.SetCaptureBufferDurationMs(40);
  model.SetRenderBufferDurationMs(40);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.active_requested_timing_present = true;
  runningStats.requested_monitor_delay_ms = 120;
  runningStats.requested_capture_buffer_duration_ms = 40;
  runningStats.requested_render_buffer_duration_ms = 40;
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetFixedDelayMs(250);
  model.SetCaptureBufferDurationMs(60);
  model.SetRenderBufferDurationMs(80);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(summary, L"Follow defaults: On") &&
         Contains(summary, L"Monitor delay: 250 ms") &&
         Contains(summary, L"Capture buffer: 60 ms") &&
         Contains(summary, L"Render buffer: 80 ms") &&
         Contains(diagnostics, L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(diagnostics, L"Active monitor delay: 120 ms") &&
         Contains(diagnostics, L"Active capture buffer: 40 ms") &&
         Contains(diagnostics, L"Active render buffer: 40 ms") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestRunningWasapiModeConfigEditPreservesActiveWasapiModeDiagnostics() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureWasapiShareMode(WasapiShareMode::Shared);
  model.SetCaptureWasapiDriveMode(WasapiDriveMode::EventDriven);
  model.SetRenderWasapiShareMode(WasapiShareMode::Shared);
  model.SetRenderWasapiDriveMode(WasapiDriveMode::EventDriven);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.active_requested_wasapi_mode_present = true;
  runningStats.requested_capture_wasapi_mode = L"Shared / Event";
  runningStats.requested_render_wasapi_mode = L"Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureWasapiShareMode(WasapiShareMode::Exclusive);
  model.SetCaptureWasapiDriveMode(WasapiDriveMode::TimerDriven);
  model.SetRenderWasapiShareMode(WasapiShareMode::Exclusive);
  model.SetRenderWasapiDriveMode(WasapiDriveMode::TimerDriven);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(summary, L"Capture WASAPI: Exclusive / Timer") &&
         Contains(summary, L"Render WASAPI: Exclusive / Timer") &&
         Contains(diagnostics, L"Active capture WASAPI request: Shared / Event") &&
         Contains(diagnostics, L"Active render WASAPI request: Shared / Event") &&
         !Contains(diagnostics, L"Active capture WASAPI request: Exclusive / Timer") &&
         !Contains(diagnostics, L"Active render WASAPI request: Exclusive / Timer") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestFollowDefaultsRunningWasapiModeConfigEditPreservesTrackingAndActiveModes() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);
  model.SetCaptureWasapiShareMode(WasapiShareMode::Shared);
  model.SetCaptureWasapiDriveMode(WasapiDriveMode::EventDriven);
  model.SetRenderWasapiShareMode(WasapiShareMode::Shared);
  model.SetRenderWasapiDriveMode(WasapiDriveMode::EventDriven);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.active_requested_wasapi_mode_present = true;
  runningStats.requested_capture_wasapi_mode = L"Shared / Event";
  runningStats.requested_render_wasapi_mode = L"Shared / Event";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureWasapiShareMode(WasapiShareMode::Exclusive);
  model.SetCaptureWasapiDriveMode(WasapiDriveMode::TimerDriven);
  model.SetRenderWasapiShareMode(WasapiShareMode::Exclusive);
  model.SetRenderWasapiDriveMode(WasapiDriveMode::TimerDriven);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(summary, L"Follow defaults: On") &&
         Contains(summary, L"Capture WASAPI: Exclusive / Timer") &&
         Contains(summary, L"Render WASAPI: Exclusive / Timer") &&
         Contains(diagnostics, L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(diagnostics, L"Active capture WASAPI request: Shared / Event") &&
         Contains(diagnostics, L"Active render WASAPI request: Shared / Event") &&
         !Contains(diagnostics, L"Active capture WASAPI request: Exclusive / Timer") &&
         !Contains(diagnostics, L"Active render WASAPI request: Exclusive / Timer") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestRunningCaptureConfigEditPreservesActiveRequestedDiagnostics() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSampleRate(48000);
  model.SetCaptureChannels(2);
  model.SetCaptureSampleType(AudioSampleType::Float32);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureSampleRate(44100);

  const auto state = model.session_state();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return state == L"Running" &&
         Contains(diagnostics, L"Current configured capture: 44100 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestFollowDefaultsRunningCaptureConfigEditPreservesTrackingAndActiveRequest() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);
  model.SetCaptureSampleRate(48000);
  model.SetCaptureChannels(2);
  model.SetCaptureSampleType(AudioSampleType::Float32);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetCaptureSampleRate(44100);

  const auto state = model.session_state();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return state == L"Running" &&
         Contains(diagnostics, L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(diagnostics, L"Current configured capture: 44100 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestRunningRenderConfigEditPreservesActiveRequestedDiagnostics() {
  AppModel model = MakeStubBackedModel();
  model.SetAutoAlignRenderFormat(false);
  model.SetRenderSampleRate(48000);
  model.SetRenderChannels(2);
  model.SetRenderSampleType(AudioSampleType::Float32);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetRenderSampleRate(44100);

  const auto state = model.session_state();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return state == L"Running" &&
         Contains(diagnostics, L"Current configured render: 44100 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestFollowDefaultsRunningRenderConfigEditPreservesTrackingAndActiveRequest() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);
  model.SetAutoAlignRenderFormat(false);
  model.SetRenderSampleRate(48000);
  model.SetRenderChannels(2);
  model.SetRenderSampleType(AudioSampleType::Float32);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.SetRenderSampleRate(44100);

  const auto state = model.session_state();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return state == L"Running" &&
         Contains(diagnostics, L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(diagnostics, L"Current configured render: 44100 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestSummaryMentionsAutoDumpPathWhenEnabledWithoutPath() {
  AppModel model = MakeStubBackedModel();
  model.SetDumpEnabled(true);
  model.SetDumpPath(L"");

  const auto summary = model.summary_text();
  return Contains(summary, L"Dump: On (WAV)") &&
         Contains(summary, L"Dump path: Auto temp file");
}

bool TestFollowDefaultRefreshUpdatesSelectedIds() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);

  DeviceEnumerationSnapshot first;
  first.capture_devices.push_back(
      {L"cap-a", L"Mic A", true, AudioDirection::Capture, false, 0});
  first.render_devices.push_back(
      {L"ren-a", L"Speaker A", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(first);

  DeviceEnumerationSnapshot second;
  second.capture_devices.push_back(
      {L"cap-b", L"Mic B", true, AudioDirection::Capture, false, 0});
  second.render_devices.push_back(
      {L"ren-b", L"Speaker B", true, AudioDirection::Render, true, 0});
  model.HandleDefaultDeviceRefresh();
  model.OnDevicesUpdated(second);

  const auto config = model.configuration();
  const auto diagnostics = model.diagnostics_text();
  return config.capture.device_id == L"cap-b" &&
         config.render.device_id == L"ren-b" &&
         Contains(diagnostics, L"Device tracking: current capture/render selection follows system defaults");
}

bool TestFollowDefaultsRefreshDevicesResyncsTrackedIds() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);

  DeviceEnumerationSnapshot first;
  first.capture_devices.push_back(
      {L"cap-a", L"Mic A", true, AudioDirection::Capture, false, 0});
  first.render_devices.push_back(
      {L"ren-a", L"Speaker A", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(first);

  DeviceEnumerationSnapshot second;
  second.capture_devices.push_back(
      {L"cap-b", L"Mic B", true, AudioDirection::Capture, false, 0});
  second.render_devices.push_back(
      {L"ren-b", L"Speaker B", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(second);
  model.RefreshDevices();

  const auto config = model.configuration();
  return config.capture.device_id == L"WASAPI:Capture:Default Capture" &&
         config.render.device_id == L"WASAPI:Render:Default Render";
}

bool TestFollowDefaultsRefreshDevicesWhileRunningRecordsRebuild() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");

  model.RefreshDevices(true);

  const auto state = model.session_state();
  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return state == L"Running" &&
         Contains(summary,
                  L"Running session note: the active session was rebuilt successfully after the device change.") &&
         Contains(diagnostics, L"Last device change: refresh-devices => rebuild-success") &&
         Contains(diagnostics, L"Last rebuild: refresh-devices => success");
}

bool TestDefaultDeviceRefreshWithoutFollowDefaultsDoesNotRecordRebuild() {
  AppModel model = MakeStubBackedModel();

  DeviceEnumerationSnapshot first;
  first.capture_devices.push_back(
      {L"cap-a", L"Mic A", true, AudioDirection::Capture, false, 0});
  first.render_devices.push_back(
      {L"ren-a", L"Speaker A", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(first);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.last_rebuild_reason = L"refresh-devices";
  runningStats.last_rebuild_result = L"success";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");
  model.HandleDefaultDeviceRefresh();

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  const auto state = model.session_state();
  model.OnSessionStateChanged(L"Stopped");

  return state == L"Running" &&
         Contains(summary,
                  L"Running session note: the default-device-change was tracked, but the already-active session kept its current devices because follow-defaults is off.") &&
         Contains(diagnostics, L"Last device change: default-device-change => tracked-no-rebuild") &&
         !Contains(diagnostics, L"Last rebuild:");
}

bool TestSummaryExplainsLoopbackBackendLimits() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);
  model.SetCaptureBackend(AudioBackendType::Wasapi);
  auto summary = model.summary_text();
  const bool has_loopback_device_note =
      Contains(summary, L"Loopback capture uses render endpoints as capture sources.");
  const bool has_wasapi_note =
      Contains(summary, L"Loopback note: WASAPI loopback is shared-mode only");

  model.SetCaptureBackend(AudioBackendType::WaveApi);
  summary = model.summary_text();
  const bool has_wave_note =
      Contains(summary, L"Loopback note: WAVE loopback depends on hardware/driver devices");

  return has_loopback_device_note && has_wasapi_note && has_wave_note;
}

bool TestDiagnosticsShowDeviceCapabilityFlags() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"cap-flag");
  model.SetRenderDeviceId(L"ren-flag");

  DeviceEnumerationSnapshot snapshot;
  snapshot.capture_devices.push_back(
      {L"cap-flag", L"Flag Mic", true, AudioDirection::Capture, false,
       kDeviceCapabilitySharedMode | kDeviceCapabilityExclusiveMode |
           kDeviceCapabilityEventDriven | kDeviceCapabilityTimerDriven});
  snapshot.render_devices.push_back(
      {L"ren-flag", L"Flag Speaker", true, AudioDirection::Render, true,
       kDeviceCapabilityCallbackBuffers});
  model.OnDevicesUpdated(snapshot);

  const auto diagnostics = model.diagnostics_text();
  return Contains(diagnostics,
                  L"Selected capture device: Flag Mic | Capture | Default | Shared | Exclusive | Event | Timer") &&
         Contains(diagnostics,
                  L"Selected render device: Flag Speaker | Render | Default | Loopback | Callback");
}

bool TestDiagnosticsClarifyLoopbackCaptureDeviceLabel() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);
  model.SetCaptureDeviceId(L"loop-cap");

  DeviceEnumerationSnapshot snapshot;
  snapshot.capture_devices.push_back(
      {L"loop-cap", L"Loopback Speaker", true, AudioDirection::Render, true, 0});
  snapshot.render_devices.push_back(
      {L"render-dev", L"Render Speaker", true, AudioDirection::Render, true, 0});
  model.OnDevicesUpdated(snapshot);

  const auto diagnostics = model.diagnostics_text();
  return Contains(diagnostics,
                  L"Selected loopback capture device: Loopback Speaker | Render | Default | Loopback") &&
         Contains(diagnostics, L"Selected loopback capture id: loop-cap");
}

bool TestDiagnosticsExplainFollowDefaultsLoopbackTracking() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);
  model.SetFollowDefaultDevices(true);

  const auto diagnostics = model.diagnostics_text();
  return Contains(
      diagnostics,
      L"Device tracking: current capture/render selection follows system defaults, and loopback capture follows the current default render endpoint");
}

bool TestDiagnosticsExplainMonitorDisabled() {
  AppModel model = MakeStubBackedModel();
  model.SetMonitorEnabled(false);

  const auto diagnostics = model.diagnostics_text();
  return Contains(
      diagnostics,
      L"Render pipeline disabled: render-only settings are inactive while monitor playback is off");
}

bool TestDiagnosticsExposeEffectiveRenderRequestWhenAutoAlignEnabled() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSampleRate(44100);
  model.SetCaptureChannels(1);
  model.SetCaptureSampleType(AudioSampleType::PcmInt16);
  model.SetRenderSampleRate(48000);
  model.SetRenderChannels(2);
  model.SetRenderSampleType(AudioSampleType::Float32);
  model.SetAutoAlignRenderFormat(true);

  const auto diagnostics = model.diagnostics_text();
  return Contains(diagnostics, L"Current configured render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Effective configured render request: 44100 Hz / 1 ch / PCM16");
}

bool TestRunningMonitorConfigEditPreservesActiveRenderSemantics() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.active_render_monitor_enabled = true;
  runningStats.actual_render_backend_mode = L"WASAPI Shared / Event";
  runningStats.render_runtime_details = L"Render runtime details";
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");
  model.SetMonitorEnabled(false);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(
             summary,
             L"Render monitor playback is turned off for the next rebuilt or restarted session; the already-active render stream is still running.") &&
         Contains(
             diagnostics,
             L"Render monitor playback is disabled in the current configuration, but the already-active session still has render monitoring until the next rebuild or restart.") &&
         Contains(diagnostics, L"Active render mode: WASAPI Shared / Event") &&
         !Contains(diagnostics,
                   L"Render pipeline disabled: render-only settings are inactive while monitor playback is off");
}

bool TestRunningAutoAlignConfigEditPreservesActiveRenderRequestSemantics() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSampleRate(44100);
  model.SetCaptureChannels(1);
  model.SetCaptureSampleType(AudioSampleType::PcmInt16);
  model.SetRenderSampleRate(48000);
  model.SetRenderChannels(2);
  model.SetRenderSampleType(AudioSampleType::Float32);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.active_render_monitor_enabled = true;
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");
  model.SetAutoAlignRenderFormat(true);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(summary, L"Effective render request: 44100 Hz / 1 ch / PCM16") &&
         Contains(diagnostics, L"Current configured render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Effective configured render request: 44100 Hz / 1 ch / PCM16") &&
         Contains(diagnostics, L"Active session requested render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestFollowDefaultsRunningAutoAlignConfigEditPreservesTrackingAndActiveRenderRequest() {
  AppModel model = MakeStubBackedModel();
  model.SetFollowDefaultDevices(true);
  model.SetCaptureSampleRate(44100);
  model.SetCaptureChannels(1);
  model.SetCaptureSampleType(AudioSampleType::PcmInt16);
  model.SetRenderSampleRate(48000);
  model.SetRenderChannels(2);
  model.SetRenderSampleType(AudioSampleType::Float32);

  SessionRuntimeStats runningStats;
  runningStats.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  runningStats.requested_render_format = L"48000 Hz / 2 ch / Float32";
  runningStats.active_render_monitor_enabled = true;
  model.OnStatsUpdated(runningStats);
  model.OnSessionStateChanged(L"Running");
  model.SetAutoAlignRenderFormat(true);

  const auto summary = model.summary_text();
  const auto diagnostics = model.diagnostics_text();
  model.OnSessionStateChanged(L"Stopped");

  return Contains(summary, L"Follow defaults: On") &&
         Contains(summary, L"Effective render request: 44100 Hz / 1 ch / PCM16") &&
         Contains(diagnostics, L"Device tracking: current capture/render selection follows system defaults") &&
         Contains(diagnostics, L"Current configured render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Effective configured render request: 44100 Hz / 1 ch / PCM16") &&
         Contains(diagnostics, L"Active session requested render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestRunningSessionDiagnosticsShowConfigurationNote() {
  AppModel model = MakeStubBackedModel();
  model.OnSessionStateChanged(L"Running");
  const auto diagnostics = model.diagnostics_text();
  return Contains(diagnostics,
                  L"Running session note: configuration edits update the next rebuilt or restarted session, not the already-active stream.");
}

bool TestDiagnosticsPreserveLastRebuildAcrossStatsRefresh() {
  AppModel model = MakeStubBackedModel();

  SessionRuntimeStats first;
  first.requested_capture_format = L"48000 Hz / 2 ch / Float32";
  first.requested_render_format = L"48000 Hz / 2 ch / Float32";
  first.last_device_change_reason = L"default-device-change";
  first.last_device_change_result = L"rebuild-success";
  first.last_rebuild_reason = L"default-device-change";
  first.last_rebuild_result = L"success";
  model.OnStatsUpdated(first);

  SessionRuntimeStats second;
  second.requested_capture_format = L"44100 Hz / 1 ch / PCM16";
  second.requested_render_format = L"44100 Hz / 2 ch / PCM16";
  second.actual_capture_backend_mode = L"WASAPI Shared / Event";
  second.actual_render_backend_mode = L"WAVE API Callback";
  model.OnStatsUpdated(second);

  const auto diagnostics = model.diagnostics_text();
  return Contains(diagnostics, L"Current configured capture: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Current configured render: 48000 Hz / 2 ch / Float32") &&
         Contains(diagnostics, L"Active session requested capture: 44100 Hz / 1 ch / PCM16") &&
         Contains(diagnostics, L"Active session requested render: 44100 Hz / 2 ch / PCM16") &&
         Contains(diagnostics, L"Last device change: default-device-change => rebuild-success") &&
         Contains(diagnostics, L"Last rebuild: default-device-change => success");
}

bool TestCapabilityTextExplainsCapabilitiesAndLimits() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);
  model.SetCaptureBackend(AudioBackendType::Wasapi);
  model.SetAutoAlignRenderFormat(true);

  const auto capability = model.capability_text();
  return Contains(capability, L"Current Capabilities") &&
         Contains(capability, L"WASAPI devices expose Shared/Exclusive/Event/Timer") &&
         Contains(capability, L"WAVE devices expose Callback-buffer capability") &&
         Contains(capability, L"Capture and render formats are independently configurable") &&
         Contains(capability, L"WASAPI loopback is shared-mode only") &&
         Contains(capability, L"Current Strategy") &&
         Contains(capability, L"Render auto-align makes the effective render request follow the capture format");
}

bool TestProbeTextShowsLastProbeResult() {
  AppModel model = MakeStubBackedModel();
  model.RecordProbeResult(L"start", 8, true, false, L"", 0, 0, L"success");
  const auto probe = model.probe_text();
  const bool has_render_wave =
      Contains(probe, L"render-wave=missing") ||
      Contains(probe, L"render-wave=not-started");
  return Contains(probe, L"Probe") &&
         Contains(probe, L"Last probe result: stage=start; ticks=8; capture-wave=seen;") &&
         has_render_wave &&
         Contains(probe, L"dump=none; dump-file=none; dump-bytes=0; result=success");
}

bool TestProbeTextShowsStructuredFailureFields() {
  AppModel model = MakeStubBackedModel();
  model.RecordProbeResult(L"render", 3, true, false, L"", 0, 0, L"failed",
                          L"render-write", L"Render adapter rejected audio chunk.");
  const auto probe = model.probe_text();
  return Contains(probe, L"Stage: render") &&
         Contains(probe, L"Ticks: 3") &&
         Contains(probe, L"CaptureWave: seen") &&
         Contains(probe, L"RenderWave: missing") &&
         Contains(probe, L"FailureStage: render-write") &&
         Contains(probe, L"FailureReason: Render adapter rejected audio chunk.") &&
         Contains(probe, L"Result: failed");
}

bool TestQuickProbeUpdatesProbeText() {
  AppModel model = MakeStubBackedModel();
  const auto ok = model.RunQuickProbe();
  const auto probe = model.probe_text();
  const bool has_fields = Contains(probe, L"QuickSummary:") &&
                          Contains(probe, L"Stage:") &&
                          Contains(probe, L"Ticks:") &&
                          Contains(probe, L"Waveform:") &&
                          Contains(probe, L"CaptureWave:") &&
                          Contains(probe, L"RenderWave:") &&
                          Contains(probe, L"RequestedCaptureDeviceId:") &&
                          Contains(probe, L"RequestedRenderDeviceId:") &&
                          Contains(probe, L"RequestedCapture:") &&
                          Contains(probe, L"RequestedRender:") &&
                          Contains(probe, L"RequestedCaptureMode:") &&
                          Contains(probe, L"RequestedRenderMode:") &&
                          Contains(probe, L"NegotiatedCapture:") &&
                          Contains(probe, L"NegotiatedRender:") &&
                          Contains(probe, L"CaptureFormatMatch:") &&
                          Contains(probe, L"RenderFormatMatch:") &&
                          Contains(probe, L"CaptureMode:") &&
                          Contains(probe, L"RenderMode:") &&
                          Contains(probe, L"Dump:") &&
                          Contains(probe, L"DumpFile:") &&
                          Contains(probe, L"DumpBytes:") &&
                          Contains(probe, L"DumpStatus:") &&
                          Contains(probe, L"Resampler:") &&
                          Contains(probe, L"CaptureRuntime:") &&
                          Contains(probe, L"RenderRuntime:") &&
                          Contains(probe, L"RenderUpdates:");
  if (!has_fields) {
    std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
    return false;
  }
  if (!Contains(probe, L"DumpBytes: 0") &&
      Contains(probe, L"CaptureWave: missing")) {
    std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
    return false;
  }
  if (!Contains(probe, L"RenderUpdates: 0") &&
      Contains(probe, L"RenderWave: missing")) {
    std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
    return false;
  }
  if (ok && (!Contains(probe, L"CaptureMode: WASAPI ") ||
             !Contains(probe, L"RenderMode: WASAPI ") ||
             !Contains(probe, L"DumpStatus: data"))) {
    std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
    return false;
  }
  if (ok && (!Contains(probe, L"RequestedCaptureMode: WASAPI Shared / Event") ||
             !Contains(probe, L"RequestedRenderMode: WASAPI Shared / Event"))) {
    std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
    return false;
  }
  if (ok && (!Contains(probe, L"RequestedCapture:") ||
             !Contains(probe, L"RequestedRender:") ||
             !Contains(probe, L"NegotiatedCapture:") ||
             !Contains(probe, L"NegotiatedRender:") ||
             !Contains(probe, L"CaptureFormatMatch:") ||
             !Contains(probe, L"RenderFormatMatch:"))) {
    std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
    return false;
  }
  if (ok && (!Contains(probe, L"QuickSummary: success") ||
             !Contains(probe, L"dump=data") ||
             !Contains(probe, L"cap-fmt=") ||
             !Contains(probe, L"ren-fmt=") ||
             !Contains(probe, L"mode=matched") ||
             !Contains(probe, L"cap-wave=seen") ||
             !Contains(probe, L"ren-wave=seen") ||
             !Contains(probe, L"ren-updates="))) {
    std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
    return false;
  }
  if (!ok) {
    const bool has_failure_evidence =
        Contains(probe, L"QuickSummary: failed") ||
        Contains(probe, L"QuickSummary: failed-during-tick") ||
        Contains(probe, L"FailureStage:") && !Contains(probe, L"FailureStage: none");
    if (!has_failure_evidence) {
      std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
      return false;
    }
  }
  if (Contains(probe, L"RenderWave: missing")) {
    const bool result = !ok &&
                        !Contains(probe, L"FailureStage: none") &&
                        (Contains(probe, L"Result: failed") ||
                         Contains(probe, L"Result: failed-during-tick"));
    if (!result) {
      std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
    }
    return result;
  }
  const bool result = (ok && Contains(probe, L"Result: success")) ||
                      (!ok && (Contains(probe, L"Result: failed") ||
                               Contains(probe, L"Result: failed-during-tick")));
  if (!result) {
    std::wcerr << L"QUICK_PROBE_TEXT:\n" << probe << L"\n";
  }
  return result;
}

bool TestQuickProbeSummaryModeMatchSupportsWaveApi() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureBackend(AudioBackendType::WaveApi);
  model.SetRenderBackend(AudioBackendType::WaveApi);
  model.SetFixedDelayMs(0);

  const bool ok = model.RunQuickProbe();
  const auto probe = model.probe_text();
  if (!ok) {
    return Contains(probe, L"QuickSummary: failed") ||
           Contains(probe, L"QuickSummary: failed-during-tick");
  }
  return Contains(probe, L"RequestedCaptureMode: WAVE API Callback") &&
         Contains(probe, L"RequestedRenderMode: WAVE API Callback") &&
         Contains(probe, L"CaptureMode: WAVE API Callback") &&
         Contains(probe, L"RenderMode: WAVE API Callback") &&
         Contains(probe, L"QuickSummary: success") &&
         Contains(probe, L"mode=matched");
}

bool TestQuickProbeSummaryExplainsMonitorDisabled() {
  AppModel model = MakeStubBackedModel();
  model.SetMonitorEnabled(false);
  model.SetRenderDeviceId(L"monitor-disabled-stub-render");
  const bool ok = model.RunQuickProbe();
  const auto probe = model.probe_text();
  const auto summary = model.summary_text();
  return ok &&
         Contains(summary, L"Render pipeline disabled for monitoring; render-only settings are inactive") &&
         Contains(probe, L"QuickSummary: success") &&
         Contains(probe, L"monitor=off") &&
         Contains(probe, L"ren-wave=disabled") &&
         Contains(probe, L"ren-fmt=disabled") &&
         Contains(probe, L"mode=render-disabled") &&
         Contains(probe, L"render-wave=disabled") &&
         Contains(probe, L"RenderWave: disabled") &&
         Contains(probe, L"NegotiatedRender: disabled") &&
         Contains(probe, L"RenderFormatMatch: disabled") &&
         Contains(probe, L"RenderMode: disabled") &&
         Contains(probe, L"RenderRuntime: disabled") &&
         Contains(probe, L"RenderWaveNote: render monitoring is disabled because monitor playback is off");
}

bool TestQuickProbeMonitorDisabledIgnoresInvalidRenderDevice() {
  AppModel model = MakeStubBackedModel();
  model.SetMonitorEnabled(false);
  model.SetRenderDeviceId(L"not-a-real-render-device");

  const bool ok = model.RunQuickProbe();
  const auto probe = model.probe_text();
  return ok &&
         Contains(probe, L"QuickSummary: success") &&
         Contains(probe, L"monitor=off") &&
         Contains(probe, L"ren-fmt=disabled") &&
         Contains(probe, L"RequestedRenderDeviceId: not-a-real-render-device") &&
         Contains(probe, L"NegotiatedRender: disabled") &&
         Contains(probe, L"RenderFormatMatch: disabled") &&
         Contains(probe, L"RenderRuntime: disabled") &&
         Contains(probe, L"FailureStage: none");
}

bool TestQuickProbeStartFailureStillShowsRequestedConfig() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureBackend(AudioBackendType::WaveApi);
  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);

  const bool ok = model.RunQuickProbe();
  const auto probe = model.probe_text();
  const auto capture_id = model.configuration().capture.device_id;
  const auto render_id = model.configuration().render.device_id;
  const bool has_capture_id =
      Contains(probe, L"RequestedCaptureDeviceId: default") ||
      (!capture_id.empty() &&
       Contains(probe, L"RequestedCaptureDeviceId: " + capture_id));
  const bool has_render_id =
      Contains(probe, L"RequestedRenderDeviceId: default") ||
      (!render_id.empty() &&
       Contains(probe, L"RequestedRenderDeviceId: " + render_id));
  return !ok &&
         Contains(probe, L"QuickSummary: failed | dump=none | cap-fmt=not-negotiated | ren-fmt=not-negotiated | mode=not-started") &&
         Contains(probe, L"FailureStage: source-mode") &&
         has_capture_id &&
         has_render_id &&
         Contains(probe, L"RequestedCapture: 48000 Hz / 2 ch / Float32") &&
         Contains(probe, L"RequestedRender: 48000 Hz / 2 ch / Float32") &&
         Contains(probe, L"RequestedCaptureMode: WAVE API Callback") &&
         Contains(probe, L"RequestedRenderMode: WASAPI Shared / Event") &&
         Contains(probe, L"Waveform: not-started") &&
         Contains(probe, L"CaptureWave: not-started") &&
         Contains(probe, L"RenderWave: not-started") &&
         Contains(probe, L"NegotiatedCapture: not-negotiated") &&
         Contains(probe, L"NegotiatedRender: not-negotiated") &&
         Contains(probe, L"CaptureFormatMatch: not-negotiated") &&
         Contains(probe, L"RenderFormatMatch: not-negotiated") &&
         Contains(probe, L"CaptureMode: not-started") &&
         Contains(probe, L"RenderMode: not-started") &&
         Contains(probe, L"Resampler: not-started") &&
         Contains(probe, L"CaptureRuntime: not-started") &&
         Contains(probe, L"RenderRuntime: not-started");
}

bool TestQuickProbeReportsRequestedDeviceIds() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"cap-dev-explicit");
  model.SetRenderDeviceId(L"ren-dev-explicit");

  model.RunQuickProbe();
  const auto probe = model.probe_text();
  return Contains(probe, L"RequestedCaptureDeviceId: cap-dev-explicit") &&
         Contains(probe, L"RequestedRenderDeviceId: ren-dev-explicit");
}

bool TestQuickProbeExplainsLoopbackDeviceSelectionMismatch() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureSourceMode(AudioSourceMode::SystemLoopback);
  model.SetCaptureDeviceId(L"not-a-loopback-render-endpoint");

  const bool ok = model.RunQuickProbe();
  const auto probe = model.probe_text();
  const bool has_loopback_hint =
      Contains(probe, L"Selected loopback capture device is not available for this source.") &&
      Contains(probe, L"Use devices --source=loopback to choose a render-backed loopback endpoint.");
  return !ok &&
         Contains(probe, L"FailureStage: source-mode") &&
         has_loopback_hint &&
         Contains(probe, L"RequestedCaptureDeviceId: not-a-loopback-render-endpoint");
}

bool TestQuickProbeExplainsApplicationLoopbackTargetFailure() {
  AppModel model;
  if (!model.Initialize()) {
    return false;
  }
  model.SetCaptureSourceMode(AudioSourceMode::ApplicationLoopback);
  model.SetApplicationLoopbackProcess(L"1234");

  const bool ok = model.RunQuickProbe();
  const auto probe = model.probe_text();
  return !ok &&
         Contains(probe, L"FailureStage: format-resolution") &&
         Contains(probe,
                  L"Application loopback is not supported on this machine. Windows process loopback capture requires client build 20348 or newer.");
}

bool TestQuickProbeDoesNotDuplicateSharedEnginePeriodDetails() {
  AppModel model = MakeStubBackedModel();
  model.RunQuickProbe();
  const auto probe = model.probe_text();
  const auto occurrences =
      CountOccurrences(probe, L"sharedEnginePeriodFrames=");
  return occurrences <= 2;
}

bool TestStoppedSessionClearsWaveformCaches() {
  AppModel model;
  const std::vector<WaveformEnvelopePoint> waveform = {
      {.min_value = -0.25f, .max_value = 0.5f}};
  const MeterValues meter {.peak = 0.5f, .rms = 0.25f, .dbfs = -6.0f, .clipping = false};

  model.OnWaveformUpdated(AudioDirection::Capture, waveform, meter);
  model.OnWaveformUpdated(AudioDirection::Render, waveform, meter);
  if (model.capture_waveform().empty() || model.render_waveform().empty()) {
    return false;
  }

  model.OnSessionStateChanged(L"Stopped");
  return model.capture_waveform().empty() && model.render_waveform().empty();
}

bool TestStoppedSessionNormalizesToIdleState() {
  AppModel model = MakeStubBackedModel();
  model.OnSessionStateChanged(L"Running");
  model.OnSessionStateChanged(L"Stopped");
  return model.session_state() == L"Idle";
}

bool TestQuickProbeRestoresRunningSession() {
  AppModel model = MakeStubBackedModel();
  if (!model.Start()) {
    return false;
  }
  model.RunQuickProbe();
  const auto state = model.session_state();
  model.Stop();
  return state == L"Running";
}

bool TestProbeTextShowsProbeMatrixLines() {
  AppModel model;
  model.RecordProbeBatchResult(
      {L"Microphone | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WASAPI: PASS | cap=48000 Hz / 2 ch / PCM16 | ren=48000 Hz / 2 ch / PCM16 | cap-fmt-match=matched | ren-fmt-match=adjusted | cap-req=WASAPI Shared / Event | ren-req=WASAPI Exclusive / Timer | cap-mode=WASAPI Shared / Event | ren-mode=WASAPI Exclusive / Timer | mode-match=adjusted | dump-bytes=1024 | dump-status=data | capture-wave=seen | render-wave=seen",
       L"SystemLoopback | align=on | profile=PCM24-44k-mono | WASAPI -> WAVE API: FAIL | cap-req=WASAPI Shared / Event | ren-req=WAVE API Callback [source-mode] {Selected backend does not support the chosen capture source. Use --capture-backend=wasapi for loopback, or switch --source=mic.}",
       L"SystemLoopback | align=off | delay=120ms | buf=cap80-ren120 | profile=PCM16-48k-stereo | WASAPI -> WAVE API: CAPTURE_MISSING | cap-fmt-match=matched | ren-fmt-match=matched | cap-req=WASAPI Shared / Event | ren-req=WAVE API Callback | mode-match=matched | render-updates=0 | capture-wave=missing | render-wave=pending-or-missing | dump-bytes=44 | dump-status=header-only",
       L"SystemLoopback | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WASAPI: FAIL | cap-req=WASAPI Exclusive / Timer | ren-req=WASAPI Shared / Event [unsupported-mode] {WASAPI loopback requires shared mode.}",
       L"Microphone | align=off | profile=PCM24-44k-mono | WASAPI -> WASAPI: FAIL | cap-req=WASAPI Shared / Event | ren-req=WASAPI Exclusive / Timer [format-resolution] {Failed to resolve runtime audio formats.}"});
  const auto probe = model.probe_text();
  const bool ok = Contains(probe, L"Last probe matrix") &&
         Contains(probe, L"MatrixSummary: PASS=1 FAIL=3 SOURCE_MODE=1 UNSUPPORTED_MODE=1 CAPTURE_DEVICE_FAIL=0 RENDER_DEVICE_FAIL=0 CAPTURE_START_FAIL=0 RENDER_START_FAIL=0 FORMAT_RESOLUTION=1 MODE_MATCHED=1 MODE_ADJUSTED=1 CAP_FMT_MATCHED=2 CAP_FMT_ADJUSTED=0 REN_FMT_MATCHED=1 REN_FMT_ADJUSTED=1 DUMP_DATA=1 DUMP_HEADER_ONLY=1 DUMP_NONE=0 CAPTURE_MISSING=1 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"MatrixHint: capture missing is clustering with source-mode failures.") &&
         Contains(probe, L"BackendSummary: WASAPI -> WASAPI PASS=1 FAIL=2 CAPTURE_MISSING=0 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"BackendSummary: WASAPI -> WAVE API PASS=0 FAIL=1 CAPTURE_MISSING=1 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"PairSummary: PCM16-48k-stereo | WASAPI -> WASAPI PASS=1 FAIL=1 CAPTURE_MISSING=0 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"PairSummary: PCM16-48k-stereo | WASAPI -> WAVE API PASS=0 FAIL=0 CAPTURE_MISSING=1 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"ProfileSummary: PCM16-48k-stereo PASS=1 FAIL=1 CAPTURE_MISSING=1 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"ProfileSummary: PCM24-44k-mono PASS=0 FAIL=2 CAPTURE_MISSING=0 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"AlignSummary: off PASS=1 FAIL=2 CAPTURE_MISSING=1 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"AlignSummary: on PASS=0 FAIL=1 CAPTURE_MISSING=0 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"SourceSummary: Microphone PASS=1 FAIL=1 CAPTURE_MISSING=0 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"SourceSummary: SystemLoopback PASS=0 FAIL=2 CAPTURE_MISSING=1 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"DelaySummary: 0ms PASS=1 FAIL=1 CAPTURE_MISSING=0 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"DelaySummary: 120ms PASS=0 FAIL=0 CAPTURE_MISSING=1 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"BufferSummary: cap40-ren40 PASS=1 FAIL=1 CAPTURE_MISSING=0 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"BufferSummary: cap80-ren120 PASS=0 FAIL=0 CAPTURE_MISSING=1 RENDER_MISSING=0 TICK_FAIL=0") &&
         Contains(probe, L"delay=0ms") &&
         Contains(probe, L"delay=120ms") &&
         Contains(probe, L"buf=cap40-ren40") &&
         Contains(probe, L"buf=cap80-ren120") &&
         Contains(probe, L"Microphone | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WASAPI: PASS | cap=48000 Hz / 2 ch / PCM16 | ren=48000 Hz / 2 ch / PCM16 | cap-fmt-match=matched | ren-fmt-match=adjusted | cap-req=WASAPI Shared / Event | ren-req=WASAPI Exclusive / Timer | cap-mode=WASAPI Shared / Event | ren-mode=WASAPI Exclusive / Timer | mode-match=adjusted | dump-bytes=1024 | dump-status=data | capture-wave=seen | render-wave=seen") &&
         Contains(probe, L"SystemLoopback | align=on | profile=PCM24-44k-mono | WASAPI -> WAVE API: FAIL | cap-req=WASAPI Shared / Event | ren-req=WAVE API Callback [source-mode] {Selected backend does not support the chosen capture source. Use --capture-backend=wasapi for loopback, or switch --source=mic.}") &&
         Contains(probe, L"SystemLoopback | align=off | delay=120ms | buf=cap80-ren120 | profile=PCM16-48k-stereo | WASAPI -> WAVE API: CAPTURE_MISSING | cap-fmt-match=matched | ren-fmt-match=matched | cap-req=WASAPI Shared / Event | ren-req=WAVE API Callback | mode-match=matched | render-updates=0 | capture-wave=missing | render-wave=pending-or-missing | dump-bytes=44 | dump-status=header-only") &&
         Contains(probe, L"SystemLoopback | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WASAPI: FAIL | cap-req=WASAPI Exclusive / Timer | ren-req=WASAPI Shared / Event [unsupported-mode] {WASAPI loopback requires shared mode.}") &&
         Contains(probe, L"Microphone | align=off | profile=PCM24-44k-mono | WASAPI -> WASAPI: FAIL | cap-req=WASAPI Shared / Event | ren-req=WASAPI Exclusive / Timer [format-resolution] {Failed to resolve runtime audio formats.}");
  if (!ok) {
    std::wcerr << L"PROBE_TEXT_SAMPLE:\n" << probe << L"\n";
  }
  return ok;
}

bool TestProbeTextTracksDeviceSelectionFailuresInMatrixSummary() {
  AppModel model;
  model.RecordProbeBatchResult(
      {L"Microphone | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WASAPI: FAIL | cap-req=WASAPI Shared / Event | ren-req=WASAPI Shared / Event | cap-dev=missing-cap | ren-dev=missing-ren [capture-device] {Selected capture device is not available for this backend/source mode.}",
       L"Microphone | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM24-44k-mono | WASAPI -> WASAPI: FAIL | cap-req=WASAPI Shared / Event | ren-req=WASAPI Shared / Event | cap-dev=default | ren-dev=missing-ren [render-device] {Selected render device is not available for this backend.}"});
  const auto probe = model.probe_text();
  return Contains(probe, L"CAPTURE_DEVICE_FAIL=1") &&
         Contains(probe, L"RENDER_DEVICE_FAIL=1");
}

bool TestProbeTextTracksRenderDeviceHintInMatrixSummary() {
  AppModel model;
  model.RecordProbeBatchResult(
      {L"System Loopback | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WAVE API: FAIL | cap-req=WASAPI Shared / Event | ren-req=WAVE API Callback | cap-dev=loop-cap | ren-dev=loop-ren [render-device] {Selected render device is not available for this backend.}",
       L"System Loopback | align=off | delay=120ms | buf=cap80-ren120 | profile=PCM24-44k-mono | WASAPI -> WAVE API: FAIL | cap-req=WASAPI Shared / Event | ren-req=WAVE API Callback | cap-dev=loop-cap | ren-dev=loop-ren [render-device] {Selected render device is not available for this backend.}"});
  const auto probe = model.probe_text();
  return Contains(probe, L"RENDER_DEVICE_FAIL=2") &&
         Contains(probe, L"MatrixHint: render-device failures are clustering.");
}

bool TestProbeTextTracksWasapiWaveCaptureMissingHint() {
  AppModel model;
  model.RecordProbeBatchResult(
      {L"System Loopback | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WAVE API: CAPTURE_MISSING | cap-fmt-match=matched | ren-fmt-match=matched | cap-req=WASAPI Shared / Event | ren-req=WAVE API Callback | mode-match=matched | render-updates=0 | capture-wave=missing | render-wave=pending-or-missing | dump-bytes=44 | dump-status=header-only",
       L"System Loopback | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM24-44k-mono | WASAPI -> WAVE API: FAIL | cap-req=WASAPI Exclusive / Timer | ren-req=WAVE API Callback [unsupported-mode] {WASAPI loopback requires shared mode.}"});
  const auto probe = model.probe_text();
  return Contains(probe, L"CAPTURE_MISSING=1") &&
         Contains(probe, L"UNSUPPORTED_MODE=1") &&
         Contains(probe, L"MatrixHint: this WASAPI-capture / WAVE-render loopback view is dominated by capture-missing and unsupported-mode results.");
}

bool TestProbeTextTracksStartFailuresInMatrixSummary() {
  AppModel model;
  model.RecordProbeBatchResult(
      {L"Microphone | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM16-48k-stereo | WASAPI -> WASAPI: FAIL | cap-req=WASAPI Shared / Event | ren-req=WASAPI Shared / Event [capture-start] {Failed to start capture adapter.}",
       L"Microphone | align=off | delay=0ms | buf=cap40-ren40 | profile=PCM24-44k-mono | WASAPI -> WASAPI: FAIL | cap-req=WASAPI Shared / Event | ren-req=WASAPI Shared / Event [render-start] {Failed to start render adapter.}"});
  const auto probe = model.probe_text();
  return Contains(probe, L"CAPTURE_START_FAIL=1") &&
         Contains(probe, L"RENDER_START_FAIL=1");
}

bool TestRunProbeMatrixReportsTicksAndWaveEvidence() {
  AppModel model = MakeStubBackedModel();
  const bool ok = model.RunProbeMatrix();
  const auto probe = model.probe_text();
  const bool has_nonzero_render_updates =
      Contains(probe, L"render-updates=1") || Contains(probe, L"render-updates=2") ||
      Contains(probe, L"render-updates=3") || Contains(probe, L"render-updates=4") ||
      Contains(probe, L"render-updates=5") || Contains(probe, L"render-updates=6") ||
      Contains(probe, L"render-updates=7") || Contains(probe, L"render-updates=8") ||
      Contains(probe, L"render-updates=9");
  if (!ok || !Contains(probe, L"ticks=") || !Contains(probe, L"capture-wave=") ||
      !Contains(probe, L"render-wave=") || !Contains(probe, L"resampler=") ||
      !Contains(probe, L"delay=") ||
      !Contains(probe, L"buf=") ||
      !Contains(probe, L"cap-fmt-match=") || !Contains(probe, L"ren-fmt-match=") ||
      !Contains(probe, L"cap-req=") || !Contains(probe, L"ren-req=") ||
      !Contains(probe, L"cap-mode=") || !Contains(probe, L"ren-mode=") ||
      !Contains(probe, L"mode-match=") ||
      !Contains(probe, L"render-updates=") || !Contains(probe, L"dump-bytes=") ||
      !Contains(probe, L"dump-status=") || !Contains(probe, L"dump-status=data") ||
      !Contains(probe, L"Exclusive / Timer") ||
      !Contains(probe, L"render-wave=seen") || !has_nonzero_render_updates ||
      !Contains(probe, L": PASS") || !Contains(probe, L"[source-mode]")) {
    std::wcerr << L"PROBE_MATRIX_TEXT:\n" << probe << L"\n";
  }
  return ok &&
         Contains(probe, L"ticks=") &&
         Contains(probe, L"capture-wave=") &&
         Contains(probe, L"render-wave=") &&
         Contains(probe, L"resampler=") &&
         Contains(probe, L"delay=") &&
         Contains(probe, L"buf=") &&
         Contains(probe, L"cap-fmt-match=") &&
         Contains(probe, L"ren-fmt-match=") &&
         Contains(probe, L"cap-req=") &&
         Contains(probe, L"ren-req=") &&
         Contains(probe, L"cap-mode=") &&
         Contains(probe, L"ren-mode=") &&
         Contains(probe, L"mode-match=") &&
         Contains(probe, L"render-updates=") &&
         Contains(probe, L"Exclusive / Timer") &&
         has_nonzero_render_updates &&
         Contains(probe, L"dump-bytes=") &&
         Contains(probe, L"dump-status=") &&
         Contains(probe, L"dump-status=data") &&
         Contains(probe, L"render-wave=seen") &&
         Contains(probe, L": PASS") &&
         Contains(probe, L"[source-mode]");
}

bool TestRunProbeMatrixReportsRequestedDeviceIds() {
  AppModel model = MakeStubBackedModel();
  model.SetCaptureDeviceId(L"cap-matrix-explicit");
  model.SetRenderDeviceId(L"ren-matrix-explicit");

  const bool ok = model.RunProbeMatrix();
  const auto probe = model.probe_text();
  return ok &&
         Contains(probe, L"cap-dev=cap-matrix-explicit") &&
         Contains(probe, L"ren-dev=ren-matrix-explicit");
}

bool TestRunProbeMatrixMayReportRenderMissingExplicitly() {
  AppModel model = MakeStubBackedModel();
  model.SetFixedDelayMs(250);
  const bool ok = model.RunProbeMatrix();
  const auto probe = model.probe_text();
  return ok && (Contains(probe, L"RENDER_MISSING") ||
                Contains(probe, L"CAPTURE_MISSING") ||
                Contains(probe, L"render-wave=seen"));
}

bool TestProbeMatrixRestoresRunningSession() {
  AppModel model = MakeStubBackedModel();
  if (!model.Start()) {
    return false;
  }
  const bool matrix_ok = model.RunProbeMatrix();
  const auto state = model.session_state();
  model.Stop();
  return matrix_ok && state == L"Running";
}

bool TestScopedProbeMatrixCanLimitToLoopback() {
  AppModel model = MakeStubBackedModel();
  const bool ok =
      model.RunProbeMatrixForSources({AudioSourceMode::SystemLoopback});
  const auto probe = model.probe_text();
  return ok &&
         !Contains(probe, L"Microphone |") &&
         Contains(probe, L"System Loopback |");
}

bool TestScopedProbeMatrixCanLimitRenderBackend() {
  AppModel model = MakeStubBackedModel();
  const bool ok = model.RunProbeMatrixForSourcesAndRenderBackends(
      {AudioSourceMode::SystemLoopback}, {AudioBackendType::WaveApi});
  const auto probe = model.probe_text();
  return ok &&
         !Contains(probe, L"WASAPI -> WASAPI") &&
         !Contains(probe, L"WAVE API -> WASAPI") &&
         Contains(probe, L"WASAPI -> WAVE API") &&
         Contains(probe, L"WAVE API -> WAVE API");
}

bool TestScopedProbeMatrixCanLimitCaptureBackend() {
  AppModel model = MakeStubBackedModel();
  const bool ok = model.RunProbeMatrixForSourcesAndBackends(
      {AudioSourceMode::SystemLoopback}, {AudioBackendType::Wasapi},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi});
  const auto probe = model.probe_text();
  return ok &&
         !Contains(probe, L"WAVE API -> WASAPI") &&
         !Contains(probe, L"WAVE API -> WAVE API") &&
         Contains(probe, L"WASAPI -> WASAPI") &&
         Contains(probe, L"WASAPI -> WAVE API");
}

bool TestScopedProbeMatrixCanLimitAlignMode() {
  AppModel model = MakeStubBackedModel();
  const bool ok = model.RunProbeMatrixFiltered(
      {AudioSourceMode::SystemLoopback},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {WasapiShareMode::Shared, WasapiShareMode::Exclusive},
      {true},
      {L"PCM16-48k-stereo", L"PCM24-44k-mono"});
  const auto probe = model.probe_text();
  return ok &&
         !Contains(probe, L"align=off") &&
         Contains(probe, L"align=on");
}

bool TestScopedProbeMatrixCanLimitDelay() {
  AppModel model = MakeStubBackedModel();
  const bool ok = model.RunProbeMatrixFiltered(
      {AudioSourceMode::SystemLoopback},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {WasapiShareMode::Shared, WasapiShareMode::Exclusive},
      {false, true},
      {L"PCM16-48k-stereo", L"PCM24-44k-mono"},
      {L"0ms"});
  const auto probe = model.probe_text();
  return ok &&
         !Contains(probe, L"delay=120ms") &&
         Contains(probe, L"delay=0ms");
}

bool TestScopedProbeMatrixCanLimitBufferProfile() {
  AppModel model = MakeStubBackedModel();
  const bool ok = model.RunProbeMatrixFiltered(
      {AudioSourceMode::SystemLoopback},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {WasapiShareMode::Shared, WasapiShareMode::Exclusive},
      {false, true},
      {L"PCM16-48k-stereo", L"PCM24-44k-mono"},
      {L"0ms", L"120ms"},
      {L"cap40-ren40"});
  const auto probe = model.probe_text();
  return ok &&
         !Contains(probe, L"buf=cap80-ren120") &&
         Contains(probe, L"buf=cap40-ren40");
}

bool TestScopedProbeMatrixCanLimitProfile() {
  AppModel model = MakeStubBackedModel();
  const bool ok = model.RunProbeMatrixFiltered(
      {AudioSourceMode::SystemLoopback},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {WasapiShareMode::Shared, WasapiShareMode::Exclusive},
      {false, true},
      {L"PCM16-48k-stereo"});
  const auto probe = model.probe_text();
  return ok &&
         !Contains(probe, L"PCM24-44k-mono") &&
         Contains(probe, L"PCM16-48k-stereo");
}

bool TestScopedProbeMatrixCanLimitWasapiShareMode() {
  AppModel model = MakeStubBackedModel();
  const bool ok = model.RunProbeMatrixFiltered(
      {AudioSourceMode::SystemLoopback},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {AudioBackendType::Wasapi, AudioBackendType::WaveApi},
      {WasapiShareMode::Shared},
      {false, true},
      {L"PCM16-48k-stereo", L"PCM24-44k-mono"});
  const auto probe = model.probe_text();
  return ok &&
         !Contains(probe, L"Exclusive / Timer") &&
         Contains(probe, L"Shared / Event");
}

bool TestStubBackedLoopbackMatrixCompletesQuickly() {
  AppModel model = MakeStubBackedModel();
  const auto started = std::chrono::steady_clock::now();
  const bool ok =
      model.RunProbeMatrixForSources({AudioSourceMode::SystemLoopback});
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started)
          .count();
  return ok && elapsed < 5000;
}

bool TestRefreshDevicesDoesNotBreakActiveProbe() {
  AppModel model = MakeStubBackedModel();
  std::atomic<bool> probe_started = false;
  std::atomic<bool> keep_refreshing = true;

  std::jthread refresher([&]() {
    while (keep_refreshing.load()) {
      model.RefreshDevices();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  const auto ok = model.RunQuickProbe();
  keep_refreshing = false;
  return ok &&
         Contains(model.probe_text(), L"QuickSummary:") &&
         Contains(model.probe_text(), L"Result: success");
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    bool (*fn)();
  };

  const std::vector<NamedTest> tests = {
      {"SummaryTextReflectsConfiguration", &TestSummaryTextReflectsConfiguration},
      {"SummaryExplainsFollowDefaultsLoopbackBinding",
       &TestSummaryExplainsFollowDefaultsLoopbackBinding},
      {"SummaryShowsApplicationLoopbackTargetAndNote",
       &TestSummaryShowsApplicationLoopbackTargetAndNote},
      {"DiagnosticsExplainApplicationLoopbackTarget",
       &TestDiagnosticsExplainApplicationLoopbackTarget},
      {"CapabilityTextExplainsApplicationLoopbackLimitation",
       &TestCapabilityTextExplainsApplicationLoopbackLimitation},
      {"SummaryShowsApplicationLoopbackTargetMissingNote",
       &TestSummaryShowsApplicationLoopbackTargetMissingNote},
      {"SummaryShowsEffectiveRenderRequestWhenAutoAlignEnabled",
       &TestSummaryShowsEffectiveRenderRequestWhenAutoAlignEnabled},
      {"RunningSessionSummaryShowsConfigurationNote",
       &TestRunningSessionSummaryShowsConfigurationNote},
      {"DiagnosticsTextIncludesRuntimeAndSelectedDevices",
       &TestDiagnosticsTextIncludesRuntimeAndSelectedDevices},
      {"DiagnosticsSuppressStaleLastRebuildAfterTrackedNoRebuildChange",
       &TestDiagnosticsSuppressStaleLastRebuildAfterTrackedNoRebuildChange},
      {"DefaultDeviceSelectionBackfillsEmptyIds",
       &TestDefaultDeviceSelectionBackfillsEmptyIds},
      {"DevicesUpdatedReplacesStaleCaptureIdWithCurrentDefault",
       &TestDevicesUpdatedReplacesStaleCaptureIdWithCurrentDefault},
      {"DevicesUpdatedReplacesStaleRenderIdWithCurrentDefault",
       &TestDevicesUpdatedReplacesStaleRenderIdWithCurrentDefault},
      {"RefreshDevicesStoresReturnedSnapshotWithoutActiveSink",
       &TestRefreshDevicesStoresReturnedSnapshotWithoutActiveSink},
      {"RefreshDevicesPreservesRunningSessionState",
       &TestRefreshDevicesPreservesRunningSessionState},
      {"SourceModeChangePreservesRunningSessionState",
       &TestSourceModeChangePreservesRunningSessionState},
      {"SourceModeChangeShowsSummaryDriftNoteWhileRunning",
       &TestSourceModeChangeShowsSummaryDriftNoteWhileRunning},
      {"RenderBackendChangePreservesRunningSessionState",
       &TestRenderBackendChangePreservesRunningSessionState},
      {"RenderBackendChangeShowsSummaryDriftNoteWhileRunning",
       &TestRenderBackendChangeShowsSummaryDriftNoteWhileRunning},
      {"CaptureBackendChangePreservesRunningSessionState",
       &TestCaptureBackendChangePreservesRunningSessionState},
      {"CaptureBackendChangeShowsSummaryDriftNoteWhileRunning",
       &TestCaptureBackendChangeShowsSummaryDriftNoteWhileRunning},
      {"FollowDefaultsWhileRunningShowsSummaryTrackingNote",
       &TestFollowDefaultsWhileRunningShowsSummaryTrackingNote},
      {"FollowDefaultsSourceModeChangeShowsRunningLoopbackTrackingNote",
       &TestFollowDefaultsSourceModeChangeShowsRunningLoopbackTrackingNote},
      {"FollowDefaultsCaptureBackendChangeShowsRunningTrackingNote",
       &TestFollowDefaultsCaptureBackendChangeShowsRunningTrackingNote},
      {"FollowDefaultsRenderBackendChangeShowsRunningTrackingNote",
       &TestFollowDefaultsRenderBackendChangeShowsRunningTrackingNote},
      {"FollowDefaultsMonitorOffWhileRunningShowsCombinedSummaryAndDiagnostics",
       &TestFollowDefaultsMonitorOffWhileRunningShowsCombinedSummaryAndDiagnostics},
      {"ManualDeviceIdChangePreservesActiveRequestedDeviceIdsWhileRunning",
       &TestManualDeviceIdChangePreservesActiveRequestedDeviceIdsWhileRunning},
      {"ManualDeviceIdChangeShowsSummaryDriftNoteWhileRunning",
       &TestManualDeviceIdChangeShowsSummaryDriftNoteWhileRunning},
      {"ManualDeviceIdsPersistAcrossDevicesUpdatedWhileRunning",
       &TestManualDeviceIdsPersistAcrossDevicesUpdatedWhileRunning},
      {"RunningDumpConfigEditPreservesActiveDumpPathSemantics",
       &TestRunningDumpConfigEditPreservesActiveDumpPathSemantics},
      {"FollowDefaultsRunningDumpConfigEditPreservesTrackingAndActiveDumpPath",
       &TestFollowDefaultsRunningDumpConfigEditPreservesTrackingAndActiveDumpPath},
      {"RunningTimingConfigEditPreservesActiveTimingDiagnostics",
       &TestRunningTimingConfigEditPreservesActiveTimingDiagnostics},
      {"FollowDefaultsRunningTimingConfigEditPreservesTrackingAndActiveTiming",
       &TestFollowDefaultsRunningTimingConfigEditPreservesTrackingAndActiveTiming},
      {"RunningWasapiModeConfigEditPreservesActiveWasapiModeDiagnostics",
       &TestRunningWasapiModeConfigEditPreservesActiveWasapiModeDiagnostics},
      {"FollowDefaultsRunningWasapiModeConfigEditPreservesTrackingAndActiveModes",
       &TestFollowDefaultsRunningWasapiModeConfigEditPreservesTrackingAndActiveModes},
      {"RunningCaptureConfigEditPreservesActiveRequestedDiagnostics",
       &TestRunningCaptureConfigEditPreservesActiveRequestedDiagnostics},
      {"FollowDefaultsRunningCaptureConfigEditPreservesTrackingAndActiveRequest",
       &TestFollowDefaultsRunningCaptureConfigEditPreservesTrackingAndActiveRequest},
      {"RunningRenderConfigEditPreservesActiveRequestedDiagnostics",
       &TestRunningRenderConfigEditPreservesActiveRequestedDiagnostics},
      {"FollowDefaultsRunningRenderConfigEditPreservesTrackingAndActiveRequest",
       &TestFollowDefaultsRunningRenderConfigEditPreservesTrackingAndActiveRequest},
      {"SummaryMentionsAutoDumpPathWhenEnabledWithoutPath",
       &TestSummaryMentionsAutoDumpPathWhenEnabledWithoutPath},
      {"FollowDefaultRefreshUpdatesSelectedIds",
       &TestFollowDefaultRefreshUpdatesSelectedIds},
      {"FollowDefaultsRefreshDevicesResyncsTrackedIds",
       &TestFollowDefaultsRefreshDevicesResyncsTrackedIds},
      {"FollowDefaultsRefreshDevicesWhileRunningRecordsRebuild",
       &TestFollowDefaultsRefreshDevicesWhileRunningRecordsRebuild},
      {"DefaultDeviceRefreshWithoutFollowDefaultsDoesNotRecordRebuild",
       &TestDefaultDeviceRefreshWithoutFollowDefaultsDoesNotRecordRebuild},
      {"SummaryExplainsLoopbackBackendLimits",
       &TestSummaryExplainsLoopbackBackendLimits},
      {"DiagnosticsShowDeviceCapabilityFlags",
       &TestDiagnosticsShowDeviceCapabilityFlags},
      {"DiagnosticsClarifyLoopbackCaptureDeviceLabel",
       &TestDiagnosticsClarifyLoopbackCaptureDeviceLabel},
      {"DiagnosticsExplainFollowDefaultsLoopbackTracking",
       &TestDiagnosticsExplainFollowDefaultsLoopbackTracking},
      {"DiagnosticsExplainMonitorDisabled",
       &TestDiagnosticsExplainMonitorDisabled},
      {"DiagnosticsExposeEffectiveRenderRequestWhenAutoAlignEnabled",
       &TestDiagnosticsExposeEffectiveRenderRequestWhenAutoAlignEnabled},
      {"RunningMonitorConfigEditPreservesActiveRenderSemantics",
       &TestRunningMonitorConfigEditPreservesActiveRenderSemantics},
      {"RunningAutoAlignConfigEditPreservesActiveRenderRequestSemantics",
       &TestRunningAutoAlignConfigEditPreservesActiveRenderRequestSemantics},
      {"FollowDefaultsRunningAutoAlignConfigEditPreservesTrackingAndActiveRenderRequest",
       &TestFollowDefaultsRunningAutoAlignConfigEditPreservesTrackingAndActiveRenderRequest},
      {"RunningSessionDiagnosticsShowConfigurationNote",
       &TestRunningSessionDiagnosticsShowConfigurationNote},
      {"DiagnosticsPreserveLastRebuildAcrossStatsRefresh",
       &TestDiagnosticsPreserveLastRebuildAcrossStatsRefresh},
      {"CapabilityTextExplainsCapabilitiesAndLimits",
       &TestCapabilityTextExplainsCapabilitiesAndLimits},
      {"ProbeTextShowsLastProbeResult",
       &TestProbeTextShowsLastProbeResult},
      {"ProbeTextShowsStructuredFailureFields",
       &TestProbeTextShowsStructuredFailureFields},
      {"QuickProbeUpdatesProbeText",
       &TestQuickProbeUpdatesProbeText},
      {"QuickProbeSummaryModeMatchSupportsWaveApi",
       &TestQuickProbeSummaryModeMatchSupportsWaveApi},
      {"QuickProbeSummaryExplainsMonitorDisabled",
       &TestQuickProbeSummaryExplainsMonitorDisabled},
      {"QuickProbeMonitorDisabledIgnoresInvalidRenderDevice",
       &TestQuickProbeMonitorDisabledIgnoresInvalidRenderDevice},
      {"QuickProbeStartFailureStillShowsRequestedConfig",
       &TestQuickProbeStartFailureStillShowsRequestedConfig},
      {"QuickProbeReportsRequestedDeviceIds",
       &TestQuickProbeReportsRequestedDeviceIds},
      {"QuickProbeExplainsLoopbackDeviceSelectionMismatch",
       &TestQuickProbeExplainsLoopbackDeviceSelectionMismatch},
      {"QuickProbeExplainsApplicationLoopbackTargetFailure",
       &TestQuickProbeExplainsApplicationLoopbackTargetFailure},
      {"QuickProbeDoesNotDuplicateSharedEnginePeriodDetails",
       &TestQuickProbeDoesNotDuplicateSharedEnginePeriodDetails},
      {"StoppedSessionClearsWaveformCaches",
       &TestStoppedSessionClearsWaveformCaches},
      {"StoppedSessionNormalizesToIdleState",
       &TestStoppedSessionNormalizesToIdleState},
      {"QuickProbeRestoresRunningSession",
       &TestQuickProbeRestoresRunningSession},
      {"ProbeTextShowsProbeMatrixLines",
       &TestProbeTextShowsProbeMatrixLines},
      {"ProbeTextTracksDeviceSelectionFailuresInMatrixSummary",
       &TestProbeTextTracksDeviceSelectionFailuresInMatrixSummary},
      {"ProbeTextTracksRenderDeviceHintInMatrixSummary",
       &TestProbeTextTracksRenderDeviceHintInMatrixSummary},
      {"ProbeTextTracksWasapiWaveCaptureMissingHint",
       &TestProbeTextTracksWasapiWaveCaptureMissingHint},
      {"ProbeTextTracksStartFailuresInMatrixSummary",
       &TestProbeTextTracksStartFailuresInMatrixSummary},
      {"RunProbeMatrixReportsTicksAndWaveEvidence",
       &TestRunProbeMatrixReportsTicksAndWaveEvidence},
      {"RunProbeMatrixReportsRequestedDeviceIds",
       &TestRunProbeMatrixReportsRequestedDeviceIds},
      {"RunProbeMatrixMayReportRenderMissingExplicitly",
       &TestRunProbeMatrixMayReportRenderMissingExplicitly},
      {"ProbeMatrixRestoresRunningSession",
       &TestProbeMatrixRestoresRunningSession},
      {"ScopedProbeMatrixCanLimitToLoopback",
       &TestScopedProbeMatrixCanLimitToLoopback},
      {"ScopedProbeMatrixCanLimitCaptureBackend",
       &TestScopedProbeMatrixCanLimitCaptureBackend},
      {"ScopedProbeMatrixCanLimitAlignMode",
       &TestScopedProbeMatrixCanLimitAlignMode},
      {"ScopedProbeMatrixCanLimitDelay",
       &TestScopedProbeMatrixCanLimitDelay},
      {"ScopedProbeMatrixCanLimitBufferProfile",
       &TestScopedProbeMatrixCanLimitBufferProfile},
      {"ScopedProbeMatrixCanLimitProfile",
       &TestScopedProbeMatrixCanLimitProfile},
      {"ScopedProbeMatrixCanLimitWasapiShareMode",
       &TestScopedProbeMatrixCanLimitWasapiShareMode},
      {"ScopedProbeMatrixCanLimitRenderBackend",
       &TestScopedProbeMatrixCanLimitRenderBackend},
      {"StubBackedLoopbackMatrixCompletesQuickly",
       &TestStubBackedLoopbackMatrixCompletesQuickly},
      {"RefreshDevicesDoesNotBreakActiveProbe",
       &TestRefreshDevicesDoesNotBreakActiveProbe},
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
