#include "stub_backends.h"

#include <cmath>

namespace winaudio {

namespace {

std::wstring MakeDeviceId(AudioBackendType backend,
                          AudioDirection direction,
                          const wchar_t* suffix) {
  return ToWideString(backend) + L":" + ToWideString(direction) + L":" + suffix;
}

AudioDeviceDescriptor MakeDevice(AudioBackendType backend,
                                 AudioDirection direction,
                                 const wchar_t* name,
                                 bool supports_loopback) {
  AudioDeviceDescriptor device;
  device.id = MakeDeviceId(backend, direction, name);
  device.friendly_name = name;
  device.direction = direction;
  device.is_default = true;
  device.supports_loopback = supports_loopback;
  return device;
}

}  // namespace

namespace {

std::optional<AudioFormatSpec> g_stub_capture_preferred_format;
std::optional<AudioFormatSpec> g_stub_render_preferred_format;
std::wstring g_stub_capture_start_error;
std::wstring g_stub_render_format_error;

}  // namespace

StubCaptureAdapter::StubCaptureAdapter(AudioBackendType backend)
    : backend_(backend) {}

std::wstring StubCaptureAdapter::name() const {
  return ToWideString(backend_) + L" Stub Capture";
}

AudioBackendType StubCaptureAdapter::backend_type() const {
  return backend_;
}

bool StubCaptureAdapter::SupportsSource(AudioSourceMode source_mode) const {
  if (backend_ == AudioBackendType::WaveApi &&
      (source_mode == AudioSourceMode::SystemLoopback ||
       source_mode == AudioSourceMode::ApplicationProcessLoopback ||
       source_mode == AudioSourceMode::ApplicationLoopback)) {
    return false;
  }
  return true;
}

std::vector<AudioDeviceDescriptor> StubCaptureAdapter::EnumerateDevices(
    AudioSourceMode source_mode) {
  if (!SupportsSource(source_mode)) {
    return {};
  }
  if (source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      source_mode == AudioSourceMode::ApplicationLoopback) {
    return {MakeDevice(backend_, AudioDirection::Capture,
                       L"Application Loopback Target", true)};
  }
  return {MakeDevice(backend_, AudioDirection::Capture, L"Default Capture", false)};
}

std::optional<AudioFormatSpec> StubCaptureAdapter::GetPreferredFormat(
    const CaptureConfig& config) {
  if (g_stub_capture_preferred_format.has_value()) {
    auto format = *g_stub_capture_preferred_format;
    format.normalize();
    return format;
  }
  auto format = config.format;
  format.normalize();
  return format;
}

bool StubCaptureAdapter::Start(const CaptureConfig& config,
                               const AudioFormatSpec& runtime_format,
                               ISessionEventSink* sink) {
  last_error_.clear();
  if (!g_stub_capture_start_error.empty()) {
    last_error_ = g_stub_capture_start_error;
    started_ = false;
    return false;
  }
  started_ = true;
  runtime_format_ = runtime_format;
  runtime_format_.normalize();
  runtime_mode_ = backend_ == AudioBackendType::Wasapi
                      ? std::wstring(L"WASAPI ") +
                            ToWideString(config.wasapi_share_mode) + L" / " +
                            ToWideString(config.wasapi_drive_mode)
                      : std::wstring(L"WAVE API Callback");
  frame_cursor_ = 0;
  if (sink != nullptr) {
    sink->OnLogLine(L"Started " + name());
  }
  return true;
}

void StubCaptureAdapter::Stop() {
  started_ = false;
}

std::optional<AudioFrameChunk> StubCaptureAdapter::ReadChunk() {
  if (!started_) {
    return std::nullopt;
  }

  AudioFrameChunk chunk;
  chunk.format = runtime_format_;
  chunk.frame_index = frame_cursor_;
  constexpr uint32_t kFrames = 480;
  chunk.interleaved_samples.resize(
      static_cast<size_t>(kFrames) * runtime_format_.channels);

  constexpr float kPi = 3.14159265358979323846f;
  for (uint32_t frame = 0; frame < kFrames; ++frame) {
    const auto phase =
        2.0f * kPi * 440.0f *
        static_cast<float>(frame_cursor_ + frame) /
        static_cast<float>(runtime_format_.sample_rate);
    const auto sample = std::sin(phase) * 0.25f;
    for (uint16_t channel = 0; channel < runtime_format_.channels; ++channel) {
      chunk.interleaved_samples[frame * runtime_format_.channels + channel] =
          sample;
    }
  }
  frame_cursor_ += kFrames;
  return chunk;
}

std::wstring StubCaptureAdapter::last_error() const {
  return last_error_;
}

std::wstring StubCaptureAdapter::runtime_mode() const {
  return runtime_mode_;
}

std::wstring StubCaptureAdapter::runtime_details() const {
  return L"Stub capture runtime";
}

StubRenderAdapter::StubRenderAdapter(AudioBackendType backend)
    : backend_(backend) {}

std::wstring StubRenderAdapter::name() const {
  return ToWideString(backend_) + L" Stub Render";
}

AudioBackendType StubRenderAdapter::backend_type() const {
  return backend_;
}

std::vector<AudioDeviceDescriptor> StubRenderAdapter::EnumerateDevices() {
  return {MakeDevice(backend_, AudioDirection::Render, L"Default Render", true)};
}

std::optional<AudioFormatSpec> StubRenderAdapter::GetPreferredFormat(
    const RenderConfig& config) {
  if (!g_stub_render_format_error.empty()) {
    return std::nullopt;
  }
  if (g_stub_render_preferred_format.has_value()) {
    auto format = *g_stub_render_preferred_format;
    format.normalize();
    return format;
  }
  auto format = config.format;
  format.normalize();
  return format;
}

bool StubRenderAdapter::Start(const RenderConfig& config,
                              const AudioFormatSpec& runtime_format,
                              ISessionEventSink* sink) {
  started_ = true;
  runtime_format_ = runtime_format;
  runtime_format_.normalize();
  runtime_mode_ = backend_ == AudioBackendType::Wasapi
                      ? std::wstring(L"WASAPI ") +
                            ToWideString(config.wasapi_share_mode) + L" / " +
                            ToWideString(config.wasapi_drive_mode)
                      : std::wstring(L"WAVE API Callback");
  written_frames_ = 0;
  if (sink != nullptr) {
    sink->OnLogLine(L"Started " + name());
  }
  return true;
}

void StubRenderAdapter::Stop() {
  started_ = false;
}

bool StubRenderAdapter::WriteChunk(const AudioFrameChunk& chunk) {
  if (!started_) {
    return false;
  }
  written_frames_ += chunk.frame_count();
  return true;
}

std::wstring StubRenderAdapter::last_error() const {
  return g_stub_render_format_error;
}

std::wstring StubRenderAdapter::runtime_mode() const {
  return runtime_mode_;
}

std::wstring StubRenderAdapter::runtime_details() const {
  return L"Stub render runtime";
}

StubAudioBackendFactory::StubAudioBackendFactory(Options options)
    : options_(std::move(options)) {}

std::unique_ptr<IAudioCaptureAdapter> StubAudioBackendFactory::CreateCaptureAdapter(
    AudioBackendType backend) {
  g_stub_capture_preferred_format = options_.capture_preferred_format;
  g_stub_capture_start_error = options_.capture_start_error;
  return std::make_unique<StubCaptureAdapter>(backend);
}

std::unique_ptr<IAudioRenderAdapter> StubAudioBackendFactory::CreateRenderAdapter(
    AudioBackendType backend) {
  g_stub_render_preferred_format = options_.render_preferred_format;
  g_stub_render_format_error = options_.render_format_error;
  return std::make_unique<StubRenderAdapter>(backend);
}

}  // namespace winaudio
