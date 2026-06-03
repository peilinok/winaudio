#pragma once

#include <string>
#include <vector>

#include "app/app_model.h"

namespace winaudio {

struct ProbeCliOptions {
  std::wstring mode = L"quick";
  SessionConfiguration config {};
  std::wstring device_name_format = L"escaped";
  std::wstring matrix_source = L"both";
  std::wstring matrix_capture_backend = L"both";
  std::wstring matrix_render_backend = L"both";
  std::wstring matrix_wasapi_share = L"both";
  std::wstring matrix_align = L"both";
  std::wstring matrix_profile = L"both";
  std::wstring matrix_delay = L"both";
  std::wstring matrix_buffer = L"both";
  std::wstring application_loopback_target_value;
};

bool ParseProbeCliOptions(const std::vector<std::wstring>& args,
                          ProbeCliOptions* options);
std::wstring BuildProbeCliUsageText();
std::wstring BuildProbeCliDeviceLine(const AudioDeviceDescriptor& device,
                                     bool escape_non_ascii = true);
std::wstring BuildProbeCliLoopbackCaptureDeviceLine(
    const AudioDeviceDescriptor& device,
    bool escape_non_ascii = true);
std::wstring NormalizeProbeCliTextForConsole(const std::wstring& text);

}  // namespace winaudio
