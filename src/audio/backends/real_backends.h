#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmeapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <audioclientactivationparams.h>
#include <wrl/client.h>

#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "audio_backend_interfaces.h"
#include "wave_format_utils.h"

namespace winaudio {

class WasapiCaptureAdapter final : public IAudioCaptureAdapter {
 public:
  WasapiCaptureAdapter() = default;

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
  static bool IsProcessLoopbackSupportedOnCurrentWindows();
  static DWORD CurrentWindowsBuildNumber();
  static std::wstring DescribeProcessLoopbackSupport();

 private:
  std::optional<AudioFormatSpec> ResolveFormat(const CaptureConfig& config,
                                               IMMDevice* device);
  bool ActivateForConfig(const CaptureConfig& config,
                         Microsoft::WRL::ComPtr<IMMDevice>* device);
  bool ActivateProcessLoopbackClient(
      const CaptureConfig& config,
      Microsoft::WRL::ComPtr<IAudioClient>* client);

  AudioSourceMode source_mode_ = AudioSourceMode::MicrophoneCapture;
  AudioFormatSpec runtime_format_ {};
  Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
  Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_client_;
  HANDLE event_handle_ = nullptr;
  uint64_t frame_cursor_ = 0;
  std::wstring last_error_;
  std::wstring runtime_mode_;
  std::wstring runtime_details_;
};

class WasapiRenderAdapter final : public IAudioRenderAdapter {
 public:
  WasapiRenderAdapter() = default;

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
  std::optional<AudioFormatSpec> ResolveFormat(const RenderConfig& config,
                                               IMMDevice* device);
  bool ActivateForConfig(const RenderConfig& config,
                         Microsoft::WRL::ComPtr<IMMDevice>* device);

  AudioFormatSpec runtime_format_ {};
  WAVEFORMATEXTENSIBLE runtime_wave_format_ {};
  Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
  Microsoft::WRL::ComPtr<IAudioRenderClient> render_client_;
  HANDLE event_handle_ = nullptr;
  UINT32 buffer_frames_ = 0;
  std::wstring last_error_;
  std::wstring runtime_mode_;
  std::wstring runtime_details_;
};

class WaveCaptureAdapter final : public IAudioCaptureAdapter {
 public:
  WaveCaptureAdapter() = default;
  ~WaveCaptureAdapter() override;

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
  struct BufferSlot {
    WAVEHDR header {};
    std::vector<BYTE> bytes;
  };

  static void CALLBACK WaveInCallback(HWAVEIN wave_in, UINT message,
                                      DWORD_PTR instance, DWORD_PTR param1,
                                      DWORD_PTR param2);
  void OnWaveInData(WAVEHDR* header);
  std::optional<AudioFormatSpec> QueryPreferredFormat(
      const CaptureConfig& config, UINT device_id);
  std::optional<UINT> ResolveDeviceId(const std::wstring& device_id) const;

  HWAVEIN wave_in_ = nullptr;
  std::vector<BufferSlot> buffers_;
  std::deque<size_t> ready_indices_;
  std::mutex mutex_;
  AudioFormatSpec runtime_format_ {};
  WAVEFORMATEXTENSIBLE runtime_wave_format_ {};
  uint64_t frame_cursor_ = 0;
  bool loopback_mode_ = false;
  std::wstring last_error_;
  std::wstring runtime_mode_;
  std::wstring runtime_details_;
};

class WaveRenderAdapter final : public IAudioRenderAdapter {
 public:
  WaveRenderAdapter() = default;
  ~WaveRenderAdapter() override;

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
  struct BufferSlot {
    WAVEHDR header {};
    std::vector<BYTE> bytes;
  };

  static void CALLBACK WaveOutCallback(HWAVEOUT wave_out, UINT message,
                                       DWORD_PTR instance, DWORD_PTR param1,
                                       DWORD_PTR param2);
  void OnWaveOutDone(WAVEHDR* header);
  std::optional<AudioFormatSpec> QueryPreferredFormat(
      const RenderConfig& config, UINT device_id);
  UINT ResolveDeviceId(const std::wstring& device_id) const;

  HWAVEOUT wave_out_ = nullptr;
  std::vector<BufferSlot> buffers_;
  std::deque<size_t> free_indices_;
  std::mutex mutex_;
  AudioFormatSpec runtime_format_ {};
  WAVEFORMATEXTENSIBLE runtime_wave_format_ {};
  std::wstring last_error_;
  std::wstring runtime_mode_;
  std::wstring runtime_details_;
};

class RealAudioBackendFactory final : public IAudioBackendFactory {
 public:
  std::unique_ptr<IAudioCaptureAdapter> CreateCaptureAdapter(
      AudioBackendType backend) override;
  std::unique_ptr<IAudioRenderAdapter> CreateRenderAdapter(
      AudioBackendType backend) override;
};

}  // namespace winaudio
