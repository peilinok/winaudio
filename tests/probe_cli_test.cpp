#include <iostream>
#include <string>
#include <vector>

#include "app/probe_cli.h"

using namespace winaudio;

namespace {

bool TestParseQuickOverrides() {
  const std::vector<std::wstring> args = {
      L"quick",
      L"--capture-backend=wave",
      L"--render-backend=wasapi",
      L"--source=loopback",
      L"--capture-rate=44100",
      L"--capture-channels=1",
      L"--capture-type=pcm16",
      L"--capture-share=exclusive",
      L"--capture-drive=timer",
      L"--render-rate=48000",
      L"--render-channels=2",
      L"--render-type=float32",
      L"--render-share=shared",
      L"--render-drive=timer",
      L"--monitor=off",
      L"--auto-align=off",
      L"--dump=on",
      L"--dump-type=pcm",
      L"--dump-path=C:\\temp\\probe_dump.pcm",
      L"--capture-device-id=cap-dev",
      L"--render-device-id=ren-dev",
      L"--delay-ms=0",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"quick" &&
         options.config.capture.backend == AudioBackendType::WaveApi &&
         options.config.render.backend == AudioBackendType::Wasapi &&
         options.config.capture.source_mode == AudioSourceMode::SystemLoopback &&
         options.config.capture.format.sample_rate == 44100 &&
         options.config.capture.format.channels == 1 &&
         options.config.capture.format.sample_type == AudioSampleType::PcmInt16 &&
         options.config.capture.wasapi_share_mode == WasapiShareMode::Exclusive &&
         options.config.capture.wasapi_drive_mode == WasapiDriveMode::TimerDriven &&
         options.config.render.format.sample_rate == 48000 &&
         options.config.render.format.channels == 2 &&
         options.config.render.format.sample_type == AudioSampleType::Float32 &&
         options.config.render.wasapi_share_mode == WasapiShareMode::Shared &&
         options.config.render.wasapi_drive_mode == WasapiDriveMode::TimerDriven &&
         options.config.render.monitor_enabled == false &&
         options.config.auto_align_render_format == false &&
         options.config.capture.dump_enabled == true &&
         options.config.capture.dump_file_type == DumpFileType::RawPcm &&
         options.config.capture.dump_path == L"C:\\temp\\probe_dump.pcm" &&
         options.config.capture.device_id == L"cap-dev" &&
         options.config.render.device_id == L"ren-dev" &&
         options.config.render.fixed_delay_ms == 0;
}

bool TestParseRejectsUnknownOverride() {
  const std::vector<std::wstring> args = {
      L"quick",
      L"--no-such-option=1",
  };

  ProbeCliOptions options;
  return !ParseProbeCliOptions(args, &options);
}

bool TestParseHelpMode() {
  const std::vector<std::wstring> args = {
      L"--help",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"--help";
}

bool TestParseDevicesMode() {
  const std::vector<std::wstring> args = {
      L"devices",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"devices";
}

bool TestParseDeviceNameFormatOverride() {
  const std::vector<std::wstring> args = {
      L"devices",
      L"--device-name-format=native",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"devices" &&
         options.device_name_format == L"native";
}

bool TestParseMatrixSourceOverride() {
  const std::vector<std::wstring> args = {
      L"matrix",
      L"--matrix-source=loopback",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"matrix" &&
         options.matrix_source == L"loopback";
}

bool TestParseMatrixRenderBackendOverride() {
  const std::vector<std::wstring> args = {
      L"matrix",
      L"--matrix-render-backend=wave",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"matrix" &&
         options.matrix_render_backend == L"wave";
}

bool TestParseMatrixCaptureBackendOverride() {
  const std::vector<std::wstring> args = {
      L"matrix",
      L"--matrix-capture-backend=wasapi",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"matrix" &&
         options.matrix_capture_backend == L"wasapi";
}

bool TestParseMatrixAlignOverride() {
  const std::vector<std::wstring> args = {
      L"matrix",
      L"--matrix-align=on",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"matrix" &&
         options.matrix_align == L"on";
}

bool TestParseMatrixProfileOverride() {
  const std::vector<std::wstring> args = {
      L"matrix",
      L"--matrix-profile=pcm24-44k-mono",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"matrix" &&
         options.matrix_profile == L"pcm24-44k-mono";
}

bool TestParseMatrixDelayOverride() {
  const std::vector<std::wstring> args = {
      L"matrix",
      L"--matrix-delay=120ms",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"matrix" &&
         options.matrix_delay == L"120ms";
}

bool TestParseMatrixBufferOverride() {
  const std::vector<std::wstring> args = {
      L"matrix",
      L"--matrix-buffer=cap80-ren120",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"matrix" &&
         options.matrix_buffer == L"cap80-ren120";
}

bool TestParseMatrixWasapiShareOverride() {
  const std::vector<std::wstring> args = {
      L"matrix",
      L"--matrix-wasapi-share=shared",
  };

  ProbeCliOptions options;
  if (!ParseProbeCliOptions(args, &options)) {
    return false;
  }

  return options.mode == L"matrix" &&
         options.matrix_wasapi_share == L"shared";
}

bool TestUsageTextIncludesDevicesAndDeviceIds() {
  const auto usage = BuildProbeCliUsageText();
  return usage.find(L"Usage: winaudio_probe.exe [quick|matrix|devices] [options]") !=
             std::wstring::npos &&
         usage.find(L"Modes:\n  quick   Run a single probe") != std::wstring::npos &&
         usage.find(L"  matrix  Run the probe matrix") != std::wstring::npos &&
         usage.find(L"  devices List available devices") != std::wstring::npos &&
         usage.find(L"--source=mic|loopback") != std::wstring::npos &&
         usage.find(L"capture device ids should match the selected capture backend/source") !=
             std::wstring::npos &&
         usage.find(L"render device ids should match the selected render backend; ignored when --monitor=off") !=
             std::wstring::npos &&
         usage.find(L"loopback capture device ids come from: devices --source=loopback") !=
             std::wstring::npos &&
         usage.find(L"--device-name-format=escaped|native") != std::wstring::npos &&
         usage.find(L"off disables the render pipeline and skips render device validation") !=
             std::wstring::npos &&
         usage.find(L"--capture-device-id=<id>") != std::wstring::npos &&
         usage.find(L"--render-device-id=<id>") != std::wstring::npos &&
         usage.find(L"--dump-path=<path>") != std::wstring::npos &&
         usage.find(L"--matrix-capture-backend=wasapi|wave|both") != std::wstring::npos &&
         usage.find(L"--matrix-align=on|off|both") != std::wstring::npos &&
         usage.find(L"--matrix-profile=pcm16-48k-stereo|pcm24-44k-mono|both") != std::wstring::npos &&
         usage.find(L"--matrix-delay=0ms|120ms|both") != std::wstring::npos &&
         usage.find(L"--matrix-buffer=cap40-ren40|cap80-ren120|both") != std::wstring::npos &&
         usage.find(L"--matrix-wasapi-share=shared|exclusive|both") != std::wstring::npos &&
         usage.find(L"--matrix-render-backend=wasapi|wave|both") != std::wstring::npos;
}

bool TestBuildProbeCliDeviceLineSanitizesFriendlyName() {
  AudioDeviceDescriptor device;
  device.id = L"dev-1";
  device.friendly_name = L"Line 1\r\n声卡";
  device.direction = AudioDirection::Render;
  device.is_default = true;
  device.supports_loopback = true;

  const auto line = BuildProbeCliDeviceLine(device);
  const bool ok =
      line.find(L"RENDER_DEVICE: \"dev-1\"") != std::wstring::npos &&
      line.find(L"| name=\"Line 1  \\u") != std::wstring::npos &&
      line.find(L"???") == std::wstring::npos &&
      line.find(L"Render | Default | Loopback") != std::wstring::npos;
  if (!ok) {
    std::wcerr << L"DEVICE_LINE:\n" << line << L"\n";
  }
  return ok;
}

bool TestBuildProbeCliDeviceLineCanPreserveUnicodeFriendlyName() {
  AudioDeviceDescriptor device;
  device.id = L"dev-2";
  device.friendly_name = L"\u9EA6\u514B\u98CE (USB)";
  device.direction = AudioDirection::Capture;

  const auto line = BuildProbeCliDeviceLine(device, false);
  return line.find(L"CAPTURE_DEVICE: \"dev-2\"") != std::wstring::npos &&
         line.find(L"name=\"\u9EA6\u514B\u98CE (USB)\"") != std::wstring::npos;
}

bool TestBuildProbeCliLoopbackCaptureDeviceLineUsesDedicatedPrefix() {
  AudioDeviceDescriptor device;
  device.id = L"loop-dev-1";
  device.friendly_name = L"Loopback Speaker";
  device.direction = AudioDirection::Render;
  device.is_default = true;
  device.supports_loopback = true;

  const auto line = BuildProbeCliLoopbackCaptureDeviceLine(device);
  return line.find(L"LOOPBACK_CAPTURE_DEVICE: \"loop-dev-1\"") !=
             std::wstring::npos &&
         line.find(L"Render | Default | Loopback") != std::wstring::npos;
}

bool TestNormalizeProbeCliTextForConsole() {
  const auto normalized =
      NormalizeProbeCliTextForConsole(L"Line1\r\nLine2\r\n\r\nLine3");
  return normalized == L"Line1\nLine2\n\nLine3";
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    bool (*fn)();
  };

  const std::vector<NamedTest> tests = {
      {"ParseQuickOverrides", &TestParseQuickOverrides},
      {"ParseRejectsUnknownOverride", &TestParseRejectsUnknownOverride},
      {"ParseHelpMode", &TestParseHelpMode},
      {"ParseDevicesMode", &TestParseDevicesMode},
      {"ParseDeviceNameFormatOverride", &TestParseDeviceNameFormatOverride},
      {"ParseMatrixSourceOverride", &TestParseMatrixSourceOverride},
      {"ParseMatrixCaptureBackendOverride", &TestParseMatrixCaptureBackendOverride},
      {"ParseMatrixAlignOverride", &TestParseMatrixAlignOverride},
      {"ParseMatrixProfileOverride", &TestParseMatrixProfileOverride},
      {"ParseMatrixDelayOverride", &TestParseMatrixDelayOverride},
      {"ParseMatrixBufferOverride", &TestParseMatrixBufferOverride},
      {"ParseMatrixWasapiShareOverride", &TestParseMatrixWasapiShareOverride},
      {"ParseMatrixRenderBackendOverride", &TestParseMatrixRenderBackendOverride},
      {"UsageTextIncludesDevicesAndDeviceIds",
       &TestUsageTextIncludesDevicesAndDeviceIds},
      {"BuildProbeCliDeviceLineSanitizesFriendlyName",
       &TestBuildProbeCliDeviceLineSanitizesFriendlyName},
      {"BuildProbeCliDeviceLineCanPreserveUnicodeFriendlyName",
       &TestBuildProbeCliDeviceLineCanPreserveUnicodeFriendlyName},
      {"BuildProbeCliLoopbackCaptureDeviceLineUsesDedicatedPrefix",
       &TestBuildProbeCliLoopbackCaptureDeviceLineUsesDedicatedPrefix},
      {"NormalizeProbeCliTextForConsole",
       &TestNormalizeProbeCliTextForConsole},
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
