#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "audio/audio_types.h"

namespace winaudio {

struct CapabilityIssue {
  std::wstring summary;
  std::wstring detail;
};

struct SessionConfiguration {
  CaptureConfig capture {};
  RenderConfig render {};
  bool follow_default_devices = false;
  bool auto_align_render_format = true;
};

struct SessionDiagnostics {
  std::vector<CapabilityIssue> issues;
  std::vector<std::wstring> log_lines;
  SessionRuntimeStats stats {};
};

struct DeviceEnumerationSnapshot {
  std::vector<AudioDeviceDescriptor> capture_devices;
  std::vector<AudioDeviceDescriptor> render_devices;
};

class ISessionEventSink {
 public:
  virtual ~ISessionEventSink() = default;
  virtual void OnLogLine(const std::wstring& line) = 0;
  virtual void OnStatsUpdated(const SessionRuntimeStats& stats) = 0;
  virtual void OnWaveformUpdated(AudioDirection direction,
                                 const std::vector<WaveformEnvelopePoint>& waveform,
                                 const MeterValues& meter) = 0;
  virtual void OnDevicesUpdated(const DeviceEnumerationSnapshot& snapshot) = 0;
  virtual void OnSessionStateChanged(const std::wstring& state) = 0;
};

class NullSessionEventSink final : public ISessionEventSink {
 public:
  void OnLogLine(const std::wstring& line) override;
  void OnStatsUpdated(const SessionRuntimeStats& stats) override;
  void OnWaveformUpdated(AudioDirection direction,
                         const std::vector<WaveformEnvelopePoint>& waveform,
                         const MeterValues& meter) override;
  void OnDevicesUpdated(const DeviceEnumerationSnapshot& snapshot) override;
  void OnSessionStateChanged(const std::wstring& state) override;
};

}  // namespace winaudio
