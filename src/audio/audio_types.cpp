#include "audio_types.h"

#include <cmath>
#include <sstream>

namespace winaudio {

void AudioFormatSpec::normalize() {
  switch (sample_type) {
    case AudioSampleType::PcmInt16:
      bits_per_sample = 16;
      break;
    case AudioSampleType::PcmInt24:
      bits_per_sample = 24;
      break;
    case AudioSampleType::PcmInt32:
    case AudioSampleType::Float32:
      bits_per_sample = 32;
      break;
  }
  block_align = static_cast<uint16_t>(channels * bytes_per_sample());
  avg_bytes_per_sec = sample_rate * block_align;
}

std::wstring ToWideString(AudioBackendType value) {
  switch (value) {
    case AudioBackendType::Wasapi:
      return L"WASAPI";
    case AudioBackendType::WaveApi:
      return L"WAVE API";
  }
  return L"Unknown";
}

std::wstring ToWideString(AudioSourceMode value) {
  switch (value) {
    case AudioSourceMode::MicrophoneCapture:
      return L"Microphone";
    case AudioSourceMode::SystemLoopback:
      return L"System Loopback";
    case AudioSourceMode::ApplicationLoopback:
      return L"Application Loopback";
  }
  return L"Unknown";
}

std::wstring ToWideString(AudioDirection value) {
  switch (value) {
    case AudioDirection::Capture:
      return L"Capture";
    case AudioDirection::Render:
      return L"Render";
  }
  return L"Unknown";
}

std::wstring ToWideString(AudioSampleType value) {
  switch (value) {
    case AudioSampleType::PcmInt16:
      return L"PCM16";
    case AudioSampleType::PcmInt24:
      return L"PCM24";
    case AudioSampleType::PcmInt32:
      return L"PCM32";
    case AudioSampleType::Float32:
      return L"Float32";
  }
  return L"Unknown";
}

std::wstring ToWideString(WasapiShareMode value) {
  switch (value) {
    case WasapiShareMode::Shared:
      return L"Shared";
    case WasapiShareMode::Exclusive:
      return L"Exclusive";
  }
  return L"Unknown";
}

std::wstring ToWideString(WasapiDriveMode value) {
  switch (value) {
    case WasapiDriveMode::EventDriven:
      return L"Event";
    case WasapiDriveMode::TimerDriven:
      return L"Timer";
  }
  return L"Unknown";
}

std::wstring ToWideString(DumpFileType value) {
  switch (value) {
    case DumpFileType::Wav:
      return L"WAV";
    case DumpFileType::RawPcm:
      return L"PCM";
  }
  return L"Unknown";
}

std::wstring DescribeAudioFormat(const AudioFormatSpec& format) {
  std::wstringstream stream;
  stream << format.sample_rate << L" Hz / " << format.channels << L" ch / "
         << ToWideString(format.sample_type);
  return stream.str();
}

std::wstring DescribeDeviceCapabilities(const AudioDeviceDescriptor& device) {
  std::wstringstream stream;
  stream << device.friendly_name << L" | " << ToWideString(device.direction);
  if (device.is_default) {
    stream << L" | Default";
  }
  if (device.supports_loopback) {
    stream << L" | Loopback";
  }
  if ((device.capability_flags & kDeviceCapabilitySharedMode) != 0) {
    stream << L" | Shared";
  }
  if ((device.capability_flags & kDeviceCapabilityExclusiveMode) != 0) {
    stream << L" | Exclusive";
  }
  if ((device.capability_flags & kDeviceCapabilityEventDriven) != 0) {
    stream << L" | Event";
  }
  if ((device.capability_flags & kDeviceCapabilityTimerDriven) != 0) {
    stream << L" | Timer";
  }
  if ((device.capability_flags & kDeviceCapabilityCallbackBuffers) != 0) {
    stream << L" | Callback";
  }
  return stream.str();
}

}  // namespace winaudio
