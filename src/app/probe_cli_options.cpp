#include "app/probe_cli.h"

namespace winaudio {

namespace {

std::wstring SanitizeForCli(const std::wstring& value) {
  std::wstring sanitized;
  sanitized.reserve(value.size());
  for (wchar_t ch : value) {
    if (ch < 0x20) {
      sanitized.push_back(L' ');
    } else {
      sanitized.push_back(ch);
    }
  }
  return sanitized;
}

std::wstring DescribeDeviceFlagsOnly(const AudioDeviceDescriptor& device) {
  auto text = SanitizeForCli(DescribeDeviceCapabilities(device));
  const auto first_sep = text.find(L" | ");
  if (first_sep == std::wstring::npos) {
    return text;
  }
  return text.substr(first_sep + 3);
}

std::wstring FriendlyNameForCli(const AudioDeviceDescriptor& device,
                                bool escape_non_ascii) {
  auto text = SanitizeForCli(device.friendly_name);
  std::wstring cli_text;
  cli_text.reserve(text.size() * (escape_non_ascii ? 6 : 1));
  for (wchar_t ch : text) {
    if (!escape_non_ascii) {
      if (ch == L'"') {
        return L"Unknown Device";
      }
      cli_text.push_back(ch);
    } else if (ch >= 0x20 && ch <= 0x7E && ch != L'\\') {
      cli_text.push_back(ch);
    } else if (ch == L' ') {
      cli_text.push_back(ch);
    } else {
      wchar_t buffer[7];
      swprintf_s(buffer, L"\\u%04X", static_cast<unsigned int>(ch));
      cli_text.append(buffer);
    }
  }

  const auto first = cli_text.find_first_not_of(L' ');
  if (first == std::wstring::npos) {
    return L"Unknown Device";
  }
  const auto last = cli_text.find_last_not_of(L' ');
  auto trimmed = cli_text.substr(first, last - first + 1);

  if (!escape_non_ascii) {
    return trimmed;
  }
  for (wchar_t ch : trimmed) {
    if (ch == L'"') {
      return L"Unknown Device";
    }
  }
  return trimmed;
}

bool ParseBackendValue(const std::wstring& value, AudioBackendType* backend) {
  if (value == L"wasapi") {
    *backend = AudioBackendType::Wasapi;
    return true;
  }
  if (value == L"wave") {
    *backend = AudioBackendType::WaveApi;
    return true;
  }
  return false;
}

bool ParseSourceValue(const std::wstring& value, AudioSourceMode* source_mode) {
  if (value == L"mic") {
    *source_mode = AudioSourceMode::MicrophoneCapture;
    return true;
  }
  if (value == L"loopback") {
    *source_mode = AudioSourceMode::SystemLoopback;
    return true;
  }
  if (value == L"app-loopback") {
    *source_mode = AudioSourceMode::ApplicationLoopback;
    return true;
  }
  return false;
}

bool ParseSampleTypeValue(const std::wstring& value, AudioSampleType* sample_type) {
  if (value == L"pcm16") {
    *sample_type = AudioSampleType::PcmInt16;
    return true;
  }
  if (value == L"pcm24") {
    *sample_type = AudioSampleType::PcmInt24;
    return true;
  }
  if (value == L"pcm32") {
    *sample_type = AudioSampleType::PcmInt32;
    return true;
  }
  if (value == L"float32") {
    *sample_type = AudioSampleType::Float32;
    return true;
  }
  return false;
}

bool ParseShareModeValue(const std::wstring& value, WasapiShareMode* share_mode) {
  if (value == L"shared") {
    *share_mode = WasapiShareMode::Shared;
    return true;
  }
  if (value == L"exclusive") {
    *share_mode = WasapiShareMode::Exclusive;
    return true;
  }
  return false;
}

bool ParseDriveModeValue(const std::wstring& value, WasapiDriveMode* drive_mode) {
  if (value == L"event") {
    *drive_mode = WasapiDriveMode::EventDriven;
    return true;
  }
  if (value == L"timer") {
    *drive_mode = WasapiDriveMode::TimerDriven;
    return true;
  }
  return false;
}

bool ParseOnOffValue(const std::wstring& value, bool* enabled) {
  if (value == L"on") {
    *enabled = true;
    return true;
  }
  if (value == L"off") {
    *enabled = false;
    return true;
  }
  return false;
}

bool ParseDumpTypeValue(const std::wstring& value, DumpFileType* file_type) {
  if (value == L"wav") {
    *file_type = DumpFileType::Wav;
    return true;
  }
  if (value == L"pcm") {
    *file_type = DumpFileType::RawPcm;
    return true;
  }
  return false;
}

}  // namespace

bool ParseProbeCliOptions(const std::vector<std::wstring>& args,
                          ProbeCliOptions* options) {
  if (options == nullptr) {
    return false;
  }

  *options = ProbeCliOptions{};
  if (!args.empty()) {
    options->mode = args.front();
  }

  for (size_t index = 1; index < args.size(); ++index) {
    const auto& arg = args[index];
    const auto separator = arg.find(L'=');
    if (separator == std::wstring::npos) {
      return false;
    }
    const auto key = arg.substr(0, separator);
    const auto value = arg.substr(separator + 1);

    if (key == L"--capture-backend") {
      if (!ParseBackendValue(value, &options->config.capture.backend)) {
        return false;
      }
    } else if (key == L"--render-backend") {
      if (!ParseBackendValue(value, &options->config.render.backend)) {
        return false;
      }
    } else if (key == L"--source") {
      if (!ParseSourceValue(value, &options->config.capture.source_mode)) {
        return false;
      }
    } else if (key == L"--capture-rate") {
      options->config.capture.format.sample_rate =
          static_cast<uint32_t>(std::stoul(value));
      options->config.capture.format.normalize();
    } else if (key == L"--capture-channels") {
      options->config.capture.format.channels =
          static_cast<uint16_t>(std::stoul(value));
      options->config.capture.format.normalize();
    } else if (key == L"--capture-type") {
      if (!ParseSampleTypeValue(value, &options->config.capture.format.sample_type)) {
        return false;
      }
      options->config.capture.format.normalize();
    } else if (key == L"--capture-share") {
      if (!ParseShareModeValue(value, &options->config.capture.wasapi_share_mode)) {
        return false;
      }
    } else if (key == L"--capture-drive") {
      if (!ParseDriveModeValue(value, &options->config.capture.wasapi_drive_mode)) {
        return false;
      }
    } else if (key == L"--render-rate") {
      options->config.render.format.sample_rate =
          static_cast<uint32_t>(std::stoul(value));
      options->config.render.format.normalize();
    } else if (key == L"--render-channels") {
      options->config.render.format.channels =
          static_cast<uint16_t>(std::stoul(value));
      options->config.render.format.normalize();
    } else if (key == L"--render-type") {
      if (!ParseSampleTypeValue(value, &options->config.render.format.sample_type)) {
        return false;
      }
      options->config.render.format.normalize();
    } else if (key == L"--render-share") {
      if (!ParseShareModeValue(value, &options->config.render.wasapi_share_mode)) {
        return false;
      }
    } else if (key == L"--render-drive") {
      if (!ParseDriveModeValue(value, &options->config.render.wasapi_drive_mode)) {
        return false;
      }
    } else if (key == L"--monitor") {
      if (!ParseOnOffValue(value, &options->config.render.monitor_enabled)) {
        return false;
      }
    } else if (key == L"--auto-align") {
      if (!ParseOnOffValue(value, &options->config.auto_align_render_format)) {
        return false;
      }
    } else if (key == L"--dump") {
      if (!ParseOnOffValue(value, &options->config.capture.dump_enabled)) {
        return false;
      }
    } else if (key == L"--dump-type") {
      if (!ParseDumpTypeValue(value, &options->config.capture.dump_file_type)) {
        return false;
      }
    } else if (key == L"--dump-path") {
      options->config.capture.dump_path = value;
    } else if (key == L"--capture-device-id") {
      options->config.capture.device_id = value;
    } else if (key == L"--render-device-id") {
      options->config.render.device_id = value;
    } else if (key == L"--app-loopback-process") {
      options->config.capture.application_loopback_process = value;
      options->application_loopback_process = value;
    } else if (key == L"--device-name-format") {
      if (value != L"escaped" && value != L"native") {
        return false;
      }
      options->device_name_format = value;
    } else if (key == L"--matrix-source") {
      if (value != L"mic" && value != L"loopback" && value != L"both") {
        return false;
      }
      options->matrix_source = value;
    } else if (key == L"--matrix-capture-backend") {
      if (value != L"wasapi" && value != L"wave" && value != L"both") {
        return false;
      }
      options->matrix_capture_backend = value;
    } else if (key == L"--matrix-wasapi-share") {
      if (value != L"shared" && value != L"exclusive" && value != L"both") {
        return false;
      }
      options->matrix_wasapi_share = value;
    } else if (key == L"--matrix-align") {
      if (value != L"on" && value != L"off" && value != L"both") {
        return false;
      }
      options->matrix_align = value;
    } else if (key == L"--matrix-profile") {
      if (value != L"pcm16-48k-stereo" && value != L"pcm24-44k-mono" &&
          value != L"both") {
        return false;
      }
      options->matrix_profile = value;
    } else if (key == L"--matrix-delay") {
      if (value != L"0ms" && value != L"120ms" && value != L"both") {
        return false;
      }
      options->matrix_delay = value;
    } else if (key == L"--matrix-buffer") {
      if (value != L"cap40-ren40" && value != L"cap80-ren120" &&
          value != L"both") {
        return false;
      }
      options->matrix_buffer = value;
    } else if (key == L"--matrix-render-backend") {
      if (value != L"wasapi" && value != L"wave" && value != L"both") {
        return false;
      }
      options->matrix_render_backend = value;
    } else if (key == L"--delay-ms") {
      options->config.render.fixed_delay_ms =
          static_cast<uint32_t>(std::stoul(value));
    } else {
      return false;
    }
  }

  return true;
}

std::wstring BuildProbeCliUsageText() {
  return L"Usage: winaudio_probe.exe [quick|matrix|devices] [options]\n"
         L"\nModes:\n"
         L"  quick   Run a single probe\n"
         L"  matrix  Run the probe matrix\n"
         L"  devices List available devices\n"
         L"\nCommon options:\n"
         L"  --capture-backend=wasapi|wave\n"
         L"  --render-backend=wasapi|wave\n"
         L"  --capture-device-id=<id>\n"
         L"    capture device ids should match the selected capture backend/source\n"
         L"  --render-device-id=<id>\n"
         L"    render device ids should match the selected render backend; ignored when --monitor=off\n"
         L"  --source=mic|loopback|app-loopback\n"
         L"    loopback capture device ids come from: devices --source=loopback\n"
         L"    app-loopback captures audio rendered by a target process tree\n"
         L"  --app-loopback-process=<name-or-pid>\n"
         L"    required with --source=app-loopback; example: spotify.exe or 1234\n"
         L"  --device-name-format=escaped|native\n"
         L"  --delay-ms=<n>\n"
         L"\nFormat options:\n"
         L"  --capture-rate=<hz>\n"
         L"  --capture-channels=<n>\n"
         L"  --capture-type=pcm16|pcm24|pcm32|float32\n"
         L"  --render-rate=<hz>\n"
         L"  --render-channels=<n>\n"
         L"  --render-type=pcm16|pcm24|pcm32|float32\n"
         L"\nMode options:\n"
         L"  --capture-share=shared|exclusive\n"
         L"  --capture-drive=event|timer\n"
         L"  --render-share=shared|exclusive\n"
         L"  --render-drive=event|timer\n"
         L"  --monitor=on|off\n"
         L"    off disables the render pipeline and skips render device validation\n"
         L"  --auto-align=on|off\n"
         L"\nDump options:\n"
         L"  --dump=on|off\n"
         L"  --dump-type=wav|pcm\n"
         L"  --dump-path=<path>\n"
         L"\nMatrix options:\n"
         L"  --matrix-source=mic|loopback|both\n"
         L"  --matrix-capture-backend=wasapi|wave|both\n"
         L"  --matrix-wasapi-share=shared|exclusive|both\n"
         L"  --matrix-align=on|off|both\n"
         L"  --matrix-profile=pcm16-48k-stereo|pcm24-44k-mono|both\n"
         L"  --matrix-delay=0ms|120ms|both\n"
         L"  --matrix-buffer=cap40-ren40|cap80-ren120|both\n"
         L"  --matrix-render-backend=wasapi|wave|both\n"
         L"\n";
}

std::wstring BuildProbeCliDeviceLine(const AudioDeviceDescriptor& device,
                                     bool escape_non_ascii) {
  const auto prefix = device.direction == AudioDirection::Capture
                          ? std::wstring(L"CAPTURE_DEVICE: ")
                          : std::wstring(L"RENDER_DEVICE: ");
  return prefix + L"\"" + SanitizeForCli(device.id) + L"\" | name=\"" +
         FriendlyNameForCli(device, escape_non_ascii) + L"\" | " +
         DescribeDeviceFlagsOnly(device);
}

std::wstring BuildProbeCliLoopbackCaptureDeviceLine(
    const AudioDeviceDescriptor& device,
    bool escape_non_ascii) {
  return std::wstring(L"LOOPBACK_CAPTURE_DEVICE: \"") +
         SanitizeForCli(device.id) + L"\" | name=\"" +
         FriendlyNameForCli(device, escape_non_ascii) +
         L"\" | " + DescribeDeviceFlagsOnly(device);
}

std::wstring NormalizeProbeCliTextForConsole(const std::wstring& text) {
  std::wstring normalized;
  normalized.reserve(text.size());
  for (size_t index = 0; index < text.size(); ++index) {
    const auto ch = text[index];
    if (ch == L'\r') {
      continue;
    }
    normalized.push_back(ch);
  }
  return normalized;
}

}  // namespace winaudio
