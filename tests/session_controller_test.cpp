#include <iostream>
#include <vector>

#include "audio/audio_session_controller.h"
#include "audio/backends/stub_backends.h"

using namespace winaudio;

namespace {

class RecordingSink final : public ISessionEventSink {
 public:
  void OnLogLine(const std::wstring& line) override {
    logs.push_back(line);
  }

  void OnStatsUpdated(const SessionRuntimeStats& stats) override {
    latest_stats = stats;
  }

  void OnWaveformUpdated(AudioDirection direction,
                         const std::vector<WaveformEnvelopePoint>& waveform,
                         const MeterValues& meter) override {
    if (direction == AudioDirection::Capture) {
      capture_waveform = waveform;
      capture_meter = meter;
    } else {
      render_waveform = waveform;
      render_meter = meter;
    }
  }

  void OnDevicesUpdated(const DeviceEnumerationSnapshot& snapshot) override {
    latest_devices = snapshot;
  }

  void OnSessionStateChanged(const std::wstring& state) override {
    states.push_back(state);
  }

  std::vector<std::wstring> logs;
  std::vector<std::wstring> states;
  SessionRuntimeStats latest_stats {};
  DeviceEnumerationSnapshot latest_devices {};
  std::vector<WaveformEnvelopePoint> capture_waveform;
  std::vector<WaveformEnvelopePoint> render_waveform;
  MeterValues capture_meter {};
  MeterValues render_meter {};
};

bool TestRefreshDevicesReturnsBothDirections() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.capture.source_mode = AudioSourceMode::MicrophoneCapture;
  config.render.backend = AudioBackendType::WaveApi;

  const auto devices = controller.RefreshDevices(config);
  return devices.capture_devices.size() == 1 && devices.render_devices.size() == 1;
}

bool TestStartTickAndStopFlow() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    std::cerr << "Initialize failed\n";
    return false;
  }

  RecordingSink sink;
  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.render.backend = AudioBackendType::WaveApi;
  config.render.fixed_delay_ms = 0;

  if (!controller.Start(config, &sink)) {
    std::wcerr << L"Start failed\n";
    return false;
  }
  if (!controller.is_running()) {
    std::cerr << "Not running after start\n";
    return false;
  }

  for (int index = 0; index < 4; ++index) {
    if (!controller.Tick()) {
      std::cerr << "Tick failed at index " << index << "\n";
      for (const auto& line : sink.logs) {
        std::wcerr << L"LOG: " << line << L"\n";
      }
      return false;
    }
  }

  controller.Stop();
  if (sink.capture_waveform.empty()) {
    std::cerr << "Capture waveform empty\n";
  }
  if (sink.render_waveform.empty()) {
    std::cerr << "Render waveform empty\n";
  }
  if (sink.states.empty()) {
    std::cerr << "No state transitions\n";
  } else {
    std::wcerr << L"First state: " << sink.states.front() << L", Last state: "
               << sink.states.back() << L"\n";
  }
  return !sink.capture_waveform.empty() && !sink.render_waveform.empty() &&
         sink.latest_stats.queue_depth_frames >= 0 &&
         !sink.states.empty() && sink.states.front() == L"Running" &&
         sink.states.back() == L"Stopped";
}

bool TestUnsupportedLoopbackIsRejectedForWaveCapture() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  RecordingSink sink;
  SessionConfiguration config;
  config.capture.backend = AudioBackendType::WaveApi;
  config.capture.source_mode = AudioSourceMode::SystemLoopback;
  config.render.backend = AudioBackendType::Wasapi;

  const auto started = controller.Start(config, &sink);
  return !started && !controller.diagnostics().issues.empty();
}

bool TestMonitorDisabledSkipsRenderPath() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  RecordingSink sink;
  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.render.backend = AudioBackendType::WaveApi;
  config.render.fixed_delay_ms = 0;
  config.render.monitor_enabled = false;

  if (!controller.Start(config, &sink)) {
    return false;
  }

  for (int index = 0; index < 3; ++index) {
    if (!controller.Tick()) {
      return false;
    }
  }

  controller.Stop();
  return !sink.capture_waveform.empty() && sink.render_waveform.empty();
}

bool TestMonitorDisabledIgnoresInvalidRenderDevice() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  RecordingSink sink;
  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.render.backend = AudioBackendType::WaveApi;
  config.render.fixed_delay_ms = 0;
  config.render.monitor_enabled = false;
  config.render.device_id = L"not-a-real-render-device";

  if (!controller.Start(config, &sink)) {
    return false;
  }

  const auto& stats = controller.diagnostics().stats;
  for (int index = 0; index < 3; ++index) {
    if (!controller.Tick()) {
      return false;
    }
  }

  controller.Stop();
  return stats.last_error_stage.empty() &&
         stats.active_render_monitor_enabled == false &&
         stats.requested_render_format == L"48000 Hz / 2 ch / Float32" &&
         sink.render_waveform.empty();
}

bool TestRenderDeviceFailureExplainsBackendMismatchRecovery() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.render.backend = AudioBackendType::Wasapi;
  config.render.device_id = L"not-a-real-render-device";

  const bool started = controller.Start(config, nullptr);
  const auto& stats = controller.diagnostics().stats;
  return !started &&
         stats.last_error_stage == L"render-device" &&
         stats.last_error_message.find(
             L"Choose a device from devices for the same render backend, or omit --render-device-id.") !=
             std::wstring::npos;
}

bool TestAutoAlignRenderFormatUsesCaptureRequest() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  RecordingSink sink;
  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.capture.format.sample_rate = 44100;
  config.capture.format.channels = 1;
  config.capture.format.sample_type = AudioSampleType::PcmInt16;
  config.capture.format.normalize();
  config.render.backend = AudioBackendType::WaveApi;
  config.render.format.sample_rate = 48000;
  config.render.format.channels = 2;
  config.render.format.sample_type = AudioSampleType::Float32;
  config.render.format.normalize();
  config.render.fixed_delay_ms = 0;
  config.auto_align_render_format = true;

  if (!controller.Start(config, &sink)) {
    return false;
  }

  const auto& stats = controller.diagnostics().stats;
  controller.Stop();
  return stats.requested_capture_format == L"44100 Hz / 1 ch / PCM16" &&
         stats.requested_render_format == L"44100 Hz / 1 ch / PCM16" &&
         stats.negotiated_render_format == L"44100 Hz / 1 ch / PCM16";
}

bool TestActualBackendModeIncludesRuntimeModeDetails() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.capture.wasapi_share_mode = WasapiShareMode::Exclusive;
  config.capture.wasapi_drive_mode = WasapiDriveMode::TimerDriven;
  config.render.backend = AudioBackendType::WaveApi;

  if (!controller.Start(config, nullptr)) {
    return false;
  }

  const auto& stats = controller.diagnostics().stats;
  controller.Stop();
  return stats.actual_capture_backend_mode == L"WASAPI Exclusive / Timer" &&
         stats.actual_render_backend_mode == L"WAVE API Callback";
}

bool TestUnsupportedWaveLoopbackShowsSourceModeReason() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::WaveApi;
  config.capture.source_mode = AudioSourceMode::SystemLoopback;
  config.render.backend = AudioBackendType::Wasapi;

  const bool started = controller.Start(config, nullptr);
  const auto& stats = controller.diagnostics().stats;
  return !started && stats.last_error_stage == L"source-mode" &&
         stats.last_error_message.find(
             L"Selected backend does not support the chosen capture source.") !=
             std::wstring::npos &&
         stats.last_error_message.find(
             L"Use --capture-backend=wasapi for loopback, or switch --source=mic.") !=
             std::wstring::npos;
}

bool TestCaptureDeviceFailureExplainsBackendSourceRecovery() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.capture.source_mode = AudioSourceMode::MicrophoneCapture;
  config.capture.device_id = L"not-a-real-capture-device";
  config.render.backend = AudioBackendType::Wasapi;

  const bool started = controller.Start(config, nullptr);
  const auto& stats = controller.diagnostics().stats;
  return !started &&
         stats.last_error_stage == L"capture-device" &&
         stats.last_error_message.find(
             L"Choose a device from devices for the same capture backend/source, or omit --capture-device-id.") !=
             std::wstring::npos;
}

bool TestLoopbackCaptureDeviceMustComeFromLoopbackEnumeration() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.capture.source_mode = AudioSourceMode::SystemLoopback;
  config.capture.device_id = L"not-a-loopback-render-endpoint";
  config.render.backend = AudioBackendType::Wasapi;

  const bool started = controller.Start(config, nullptr);
  const auto& stats = controller.diagnostics().stats;
  return !started &&
         stats.last_error_stage == L"source-mode" &&
         stats.last_error_message.find(
             L"Use devices --source=loopback to choose a render-backed loopback endpoint.") !=
             std::wstring::npos;
}

bool TestApplicationLoopbackRequiresTargetProcess() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.capture.source_mode = AudioSourceMode::ApplicationLoopback;
  config.render.backend = AudioBackendType::Wasapi;

  const bool started = controller.Start(config, nullptr);
  const auto& stats = controller.diagnostics().stats;
  return !started &&
         stats.last_error_stage == L"source-mode" &&
         stats.last_error_message.find(
             L"Application loopback requires a target application name") !=
             std::wstring::npos;
}

bool TestApplicationLoopbackCanStartWithStubWhenTargetProvided() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.capture.source_mode = AudioSourceMode::ApplicationLoopback;
  config.capture.application_loopback_target_kind =
      ApplicationLoopbackTargetKind::ApplicationName;
  config.capture.application_loopback_target_value = L"spotify.exe";
  config.render.backend = AudioBackendType::Wasapi;

  const bool started = controller.Start(config, nullptr);
  if (!started) {
    return false;
  }

  const auto& stats = controller.diagnostics().stats;
  controller.Stop();
  return stats.requested_capture_format == L"48000 Hz / 2 ch / Float32";
}

bool TestSystemLoopbackDisablesMonitorPlayback() {
  AudioSessionController controller(std::make_unique<StubAudioBackendFactory>());
  if (!controller.Initialize()) {
    return false;
  }

  SessionConfiguration config;
  config.capture.backend = AudioBackendType::Wasapi;
  config.capture.source_mode = AudioSourceMode::SystemLoopback;
  config.render.backend = AudioBackendType::Wasapi;
  config.render.monitor_enabled = true;

  const bool started = controller.Start(config, nullptr);
  if (!started) {
    return false;
  }
  const auto& stats = controller.diagnostics().stats;
  controller.Stop();
  return stats.active_render_monitor_enabled == false;
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    bool (*fn)();
  };

  const std::vector<NamedTest> tests = {
      {"RefreshDevicesReturnsBothDirections", &TestRefreshDevicesReturnsBothDirections},
      {"StartTickAndStopFlow", &TestStartTickAndStopFlow},
      {"UnsupportedLoopbackIsRejectedForWaveCapture",
       &TestUnsupportedLoopbackIsRejectedForWaveCapture},
      {"MonitorDisabledSkipsRenderPath", &TestMonitorDisabledSkipsRenderPath},
      {"MonitorDisabledIgnoresInvalidRenderDevice",
       &TestMonitorDisabledIgnoresInvalidRenderDevice},
      {"RenderDeviceFailureExplainsBackendMismatchRecovery",
       &TestRenderDeviceFailureExplainsBackendMismatchRecovery},
      {"AutoAlignRenderFormatUsesCaptureRequest",
       &TestAutoAlignRenderFormatUsesCaptureRequest},
      {"ActualBackendModeIncludesRuntimeModeDetails",
       &TestActualBackendModeIncludesRuntimeModeDetails},
      {"CaptureDeviceFailureExplainsBackendSourceRecovery",
       &TestCaptureDeviceFailureExplainsBackendSourceRecovery},
      {"UnsupportedWaveLoopbackShowsSourceModeReason",
       &TestUnsupportedWaveLoopbackShowsSourceModeReason},
      {"LoopbackCaptureDeviceMustComeFromLoopbackEnumeration",
       &TestLoopbackCaptureDeviceMustComeFromLoopbackEnumeration},
      {"SystemLoopbackDisablesMonitorPlayback",
       &TestSystemLoopbackDisablesMonitorPlayback},
      {"ApplicationLoopbackRequiresTargetProcess",
       &TestApplicationLoopbackRequiresTargetProcess},
      {"ApplicationLoopbackCanStartWithStubWhenTargetProvided",
       &TestApplicationLoopbackCanStartWithStubWhenTargetProvided},
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
