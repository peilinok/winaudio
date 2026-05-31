#include <iostream>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

#include "app/probe_cli.h"
#include "audio/com_support.h"

int wmain(int argc, wchar_t** argv) {
  using namespace winaudio;

  ScopedCoInitialize com;
  if (!com.ok()) {
    std::wcerr << L"COM init failed: " << HResultToString(com.hr()) << L"\n";
    return 1;
  }

  std::vector<std::wstring> args;
  for (int index = 1; index < argc; ++index) {
    args.push_back(argv[index]);
  }

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    std::wcerr << NormalizeProbeCliTextForConsole(BuildProbeCliUsageText());
    return 4;
  }
  if (options.mode == L"--help" || options.mode == L"help") {
    std::wcout << NormalizeProbeCliTextForConsole(BuildProbeCliUsageText());
    return 0;
  }

  AppModel model;
  if (!model.Initialize()) {
    std::wcerr << L"AppModel initialize failed\n";
    return 1;
  }

  model.SetCaptureBackend(options.config.capture.backend);
  model.SetRenderBackend(options.config.render.backend);
  model.SetCaptureSourceMode(options.config.capture.source_mode);
  model.SetCaptureDeviceId(options.config.capture.device_id);
  model.SetRenderDeviceId(options.config.render.device_id);
  model.SetCaptureSampleRate(options.config.capture.format.sample_rate);
  model.SetCaptureChannels(options.config.capture.format.channels);
  model.SetCaptureSampleType(options.config.capture.format.sample_type);
  model.SetCaptureWasapiShareMode(options.config.capture.wasapi_share_mode);
  model.SetCaptureWasapiDriveMode(options.config.capture.wasapi_drive_mode);
  model.SetRenderSampleRate(options.config.render.format.sample_rate);
  model.SetRenderChannels(options.config.render.format.channels);
  model.SetRenderSampleType(options.config.render.format.sample_type);
  model.SetRenderWasapiShareMode(options.config.render.wasapi_share_mode);
  model.SetRenderWasapiDriveMode(options.config.render.wasapi_drive_mode);
  model.SetMonitorEnabled(options.config.render.monitor_enabled);
  model.SetAutoAlignRenderFormat(options.config.auto_align_render_format);
  model.SetDumpEnabled(options.config.capture.dump_enabled);
  model.SetDumpFileType(options.config.capture.dump_file_type);
  model.SetDumpPath(options.config.capture.dump_path);
  model.SetFixedDelayMs(options.config.render.fixed_delay_ms);

  if (options.mode == L"devices") {
    if (options.device_name_format == L"native") {
      _setmode(_fileno(stdout), _O_U8TEXT);
    }
    model.RefreshDevices();
    const auto devices = model.devices();
    const bool escape_non_ascii = options.device_name_format != L"native";
    std::wcout << L"Device Query\n";
    std::wcout << NormalizeProbeCliTextForConsole(model.summary_text()) << L"\n";
    const bool loopback_capture_view =
        options.config.capture.source_mode == AudioSourceMode::SystemLoopback;
    if (loopback_capture_view) {
      std::wcout << L"Loopback Capture Devices (" << devices.capture_devices.size()
                 << L")\n";
    } else {
      std::wcout << L"Capture Devices (" << devices.capture_devices.size() << L")\n";
    }
    for (const auto& device : devices.capture_devices) {
      const auto line = loopback_capture_view
                            ? BuildProbeCliLoopbackCaptureDeviceLine(device, escape_non_ascii)
                            : BuildProbeCliDeviceLine(device, escape_non_ascii);
      std::wcout << NormalizeProbeCliTextForConsole(line) << L"\n";
    }
    std::wcout << L"Render Devices (" << devices.render_devices.size() << L")\n";
    for (const auto& device : devices.render_devices) {
      std::wcout << NormalizeProbeCliTextForConsole(BuildProbeCliDeviceLine(device, escape_non_ascii))
                 << L"\n";
    }
    return 0;
  }

  if (options.mode == L"quick") {
    const bool ok = model.RunQuickProbe();
    std::wcout << NormalizeProbeCliTextForConsole(model.probe_text()) << L"\n";
    return ok ? 0 : 2;
  }

  if (options.mode == L"matrix") {
    bool ok = false;
    std::vector<AudioBackendType> capture_backends;
    if (options.matrix_capture_backend == L"wasapi") {
      capture_backends = {AudioBackendType::Wasapi};
    } else if (options.matrix_capture_backend == L"wave") {
      capture_backends = {AudioBackendType::WaveApi};
    } else {
      capture_backends = {AudioBackendType::Wasapi, AudioBackendType::WaveApi};
    }
    std::vector<AudioBackendType> render_backends;
    if (options.matrix_render_backend == L"wasapi") {
      render_backends = {AudioBackendType::Wasapi};
    } else if (options.matrix_render_backend == L"wave") {
      render_backends = {AudioBackendType::WaveApi};
    } else {
      render_backends = {AudioBackendType::Wasapi, AudioBackendType::WaveApi};
    }
    std::vector<WasapiShareMode> wasapi_share_modes;
    if (options.matrix_wasapi_share == L"shared") {
      wasapi_share_modes = {WasapiShareMode::Shared};
    } else if (options.matrix_wasapi_share == L"exclusive") {
      wasapi_share_modes = {WasapiShareMode::Exclusive};
    } else {
      wasapi_share_modes = {WasapiShareMode::Shared, WasapiShareMode::Exclusive};
    }
    std::vector<bool> align_modes;
    if (options.matrix_align == L"on") {
      align_modes = {true};
    } else if (options.matrix_align == L"off") {
      align_modes = {false};
    } else {
      align_modes = {false, true};
    }
    std::vector<std::wstring> profile_labels;
    if (options.matrix_profile == L"pcm16-48k-stereo") {
      profile_labels = {L"PCM16-48k-stereo"};
    } else if (options.matrix_profile == L"pcm24-44k-mono") {
      profile_labels = {L"PCM24-44k-mono"};
    } else {
      profile_labels = {L"PCM16-48k-stereo", L"PCM24-44k-mono"};
    }
    std::vector<std::wstring> delay_labels;
    if (options.matrix_delay == L"0ms") {
      delay_labels = {L"0ms"};
    } else if (options.matrix_delay == L"120ms") {
      delay_labels = {L"120ms"};
    } else {
      delay_labels = {L"0ms", L"120ms"};
    }
    std::vector<std::wstring> buffer_labels;
    if (options.matrix_buffer == L"cap40-ren40") {
      buffer_labels = {L"cap40-ren40"};
    } else if (options.matrix_buffer == L"cap80-ren120") {
      buffer_labels = {L"cap80-ren120"};
    } else {
      buffer_labels = {L"cap40-ren40", L"cap80-ren120"};
    }
    if (options.matrix_source == L"mic") {
      ok = model.RunProbeMatrixFiltered(
          {AudioSourceMode::MicrophoneCapture}, capture_backends,
          render_backends, wasapi_share_modes, align_modes, profile_labels,
          delay_labels, buffer_labels);
    } else if (options.matrix_source == L"loopback") {
      ok = model.RunProbeMatrixFiltered(
          {AudioSourceMode::SystemLoopback}, capture_backends, render_backends,
          wasapi_share_modes, align_modes, profile_labels, delay_labels,
          buffer_labels);
    } else {
      ok = model.RunProbeMatrixFiltered(
          {AudioSourceMode::MicrophoneCapture,
           AudioSourceMode::SystemLoopback},
          capture_backends,
          render_backends,
          wasapi_share_modes,
          align_modes,
          profile_labels,
          delay_labels,
          buffer_labels);
    }
    std::wcout << NormalizeProbeCliTextForConsole(model.probe_text()) << L"\n";
    return ok ? 0 : 3;
  }

  std::wcerr << NormalizeProbeCliTextForConsole(BuildProbeCliUsageText());
  return 4;
}
