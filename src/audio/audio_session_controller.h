#pragma once

#include <memory>
#include <optional>

#include "audio/backends/audio_backend_interfaces.h"
#include "audio/pipeline/audio_ring_buffer.h"
#include "audio/pipeline/signal_analyzer.h"
#include "audio/pipeline/wav_dump_writer.h"
#include "audio/resample/audio_resampler.h"
#include "rtc/agora_rtc_publisher.h"

namespace winaudio {

class AudioSessionController {
 public:
  explicit AudioSessionController(
      std::unique_ptr<IAudioBackendFactory> backend_factory);

  bool Initialize();
  DeviceEnumerationSnapshot RefreshDevices(const SessionConfiguration& config);
  bool Start(const SessionConfiguration& config, ISessionEventSink* sink);
  void Stop();
  bool Tick();
  bool JoinRtc(const AgoraRtcConfig& config);
  void LeaveRtc();

  [[nodiscard]] bool is_running() const;
  [[nodiscard]] const SessionDiagnostics& diagnostics() const;
  [[nodiscard]] AgoraRtcStats rtc_stats() const;

 private:
  void Log(const std::wstring& line);
  void SetLastError(const std::wstring& stage, const std::wstring& message);
  void UpdateStats();
  std::filesystem::path BuildDefaultDumpPath() const;

  std::unique_ptr<IAudioBackendFactory> backend_factory_;
  std::unique_ptr<IAudioCaptureAdapter> capture_adapter_;
  std::unique_ptr<IAudioRenderAdapter> render_adapter_;
  std::unique_ptr<IAudioResampler> resampler_;
  std::unique_ptr<AudioRingBuffer> ring_buffer_;
  std::unique_ptr<WavDumpWriter> dump_writer_;
  std::unique_ptr<AgoraRtcPublisher> rtc_publisher_;

  SessionConfiguration config_ {};
  SessionDiagnostics diagnostics_ {};
  ISessionEventSink* sink_ = nullptr;
  SignalAnalyzer capture_analyzer_ {512};
  SignalAnalyzer render_analyzer_ {512};
  AudioFormatSpec runtime_capture_format_ {};
  AudioFormatSpec runtime_render_format_ {};
  bool running_ = false;
};

}  // namespace winaudio
