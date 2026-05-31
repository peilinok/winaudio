#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/audio_session_types.h"

namespace winaudio {

class IAudioCaptureAdapter {
 public:
  virtual ~IAudioCaptureAdapter() = default;

  virtual std::wstring name() const = 0;
  virtual AudioBackendType backend_type() const = 0;
  virtual bool SupportsSource(AudioSourceMode source_mode) const = 0;
  virtual std::vector<AudioDeviceDescriptor> EnumerateDevices(
      AudioSourceMode source_mode) = 0;
  virtual std::optional<AudioFormatSpec> GetPreferredFormat(
      const CaptureConfig& config) = 0;
  virtual bool Start(const CaptureConfig& config,
                     const AudioFormatSpec& runtime_format,
                     ISessionEventSink* sink) = 0;
  virtual void Stop() = 0;
  virtual std::optional<AudioFrameChunk> ReadChunk() = 0;
  virtual std::wstring last_error() const = 0;
  virtual std::wstring runtime_mode() const = 0;
  virtual std::wstring runtime_details() const = 0;
};

class IAudioRenderAdapter {
 public:
  virtual ~IAudioRenderAdapter() = default;

  virtual std::wstring name() const = 0;
  virtual AudioBackendType backend_type() const = 0;
  virtual std::vector<AudioDeviceDescriptor> EnumerateDevices() = 0;
  virtual std::optional<AudioFormatSpec> GetPreferredFormat(
      const RenderConfig& config) = 0;
  virtual bool Start(const RenderConfig& config,
                     const AudioFormatSpec& runtime_format,
                     ISessionEventSink* sink) = 0;
  virtual void Stop() = 0;
  virtual bool WriteChunk(const AudioFrameChunk& chunk) = 0;
  virtual std::wstring last_error() const = 0;
  virtual std::wstring runtime_mode() const = 0;
  virtual std::wstring runtime_details() const = 0;
};

class IAudioBackendFactory {
 public:
  virtual ~IAudioBackendFactory() = default;

  virtual std::unique_ptr<IAudioCaptureAdapter> CreateCaptureAdapter(
      AudioBackendType backend) = 0;
  virtual std::unique_ptr<IAudioRenderAdapter> CreateRenderAdapter(
      AudioBackendType backend) = 0;
};

}  // namespace winaudio
