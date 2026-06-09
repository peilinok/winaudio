#pragma once

#include "audio_backend_interfaces.h"

namespace winaudio {

class StubCaptureAdapter final : public IAudioCaptureAdapter {
 public:
  explicit StubCaptureAdapter(AudioBackendType backend);

  std::wstring name() const override;
  AudioBackendType backend_type() const override;
  bool SupportsSource(AudioSourceMode source_mode) const override;
  std::vector<AudioDeviceDescriptor> EnumerateDevices(
      AudioSourceMode source_mode) override;
  std::optional<AudioFormatSpec> GetPreferredFormat(
      const CaptureConfig& config) override;
  bool Start(const CaptureConfig& config,
             const AudioFormatSpec& runtime_format,
             ISessionEventSink* sink) override;
  void Stop() override;
  std::optional<AudioFrameChunk> ReadChunk() override;
  std::wstring last_error() const override;
  std::wstring runtime_mode() const override;
  std::wstring runtime_details() const override;

 private:
  AudioBackendType backend_;
  AudioFormatSpec runtime_format_ {};
  std::wstring runtime_mode_;
  std::wstring last_error_;
  bool started_ = false;
  uint64_t frame_cursor_ = 0;
};

class StubRenderAdapter final : public IAudioRenderAdapter {
 public:
  explicit StubRenderAdapter(AudioBackendType backend);

  std::wstring name() const override;
  AudioBackendType backend_type() const override;
  std::vector<AudioDeviceDescriptor> EnumerateDevices() override;
  std::optional<AudioFormatSpec> GetPreferredFormat(
      const RenderConfig& config) override;
  bool Start(const RenderConfig& config,
             const AudioFormatSpec& runtime_format,
             ISessionEventSink* sink) override;
  void Stop() override;
  bool WriteChunk(const AudioFrameChunk& chunk) override;
  std::wstring last_error() const override;
  std::wstring runtime_mode() const override;
  std::wstring runtime_details() const override;

 private:
  AudioBackendType backend_;
  AudioFormatSpec runtime_format_ {};
  std::wstring runtime_mode_;
  bool started_ = false;
  uint64_t written_frames_ = 0;
};

class StubAudioBackendFactory final : public IAudioBackendFactory {
 public:
  struct Options {
    std::optional<AudioFormatSpec> capture_preferred_format;
    std::optional<AudioFormatSpec> render_preferred_format;
    std::wstring capture_start_error;
    std::wstring render_format_error;
  };

  StubAudioBackendFactory() = default;
  explicit StubAudioBackendFactory(Options options);

  std::unique_ptr<IAudioCaptureAdapter> CreateCaptureAdapter(
      AudioBackendType backend) override;
  std::unique_ptr<IAudioRenderAdapter> CreateRenderAdapter(
      AudioBackendType backend) override;

 private:
  Options options_ {};
};

}  // namespace winaudio
