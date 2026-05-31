#include "audio_resampler.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "audio/backends/wave_format_utils.h"

namespace winaudio {

using Microsoft::WRL::ComPtr;

namespace {

class LinearAudioResampler final : public IAudioResampler {
 public:
  bool Configure(const AudioFormatSpec& input_format,
                 const AudioFormatSpec& output_format) override {
    input_format_ = input_format;
    output_format_ = output_format;
    input_format_.normalize();
    output_format_.normalize();
    configured_ = input_format_.sample_rate > 0 && output_format_.sample_rate > 0 &&
                  input_format_.channels > 0 && output_format_.channels > 0;
    return configured_;
  }

  std::optional<AudioFrameChunk> Resample(
      const AudioFrameChunk& input) override {
    if (!configured_) {
      return std::nullopt;
    }

    AudioFrameChunk output;
    output.format = output_format_;
    output.frame_index = input.frame_index;
    output.first_sample_qpc = input.first_sample_qpc;

    const auto input_frames = input.frame_count();
    if (input_frames == 0) {
      return output;
    }

    const auto output_frames = static_cast<uint32_t>(std::max<double>(
        1.0,
        std::round(static_cast<double>(input_frames) * output_format_.sample_rate /
                   input_format_.sample_rate)));
    output.interleaved_samples.resize(
        static_cast<size_t>(output_frames) * output_format_.channels);

    const auto input_channels = input_format_.channels;
    for (uint32_t out_frame = 0; out_frame < output_frames; ++out_frame) {
      const auto source_position =
          static_cast<double>(out_frame) * input_format_.sample_rate /
          output_format_.sample_rate;
      const auto left_index = static_cast<uint32_t>(
          std::min<double>(std::floor(source_position), input_frames - 1));
      const auto right_index = static_cast<uint32_t>(
          std::min<double>(left_index + 1, input_frames - 1));
      const auto fraction =
          static_cast<float>(source_position - static_cast<double>(left_index));

      for (uint16_t out_channel = 0; out_channel < output_format_.channels;
           ++out_channel) {
        const auto source_channel =
            static_cast<uint16_t>(std::min<uint16_t>(out_channel, input_channels - 1));
        const auto left_sample =
            input.interleaved_samples[left_index * input_channels + source_channel];
        const auto right_sample =
            input.interleaved_samples[right_index * input_channels + source_channel];
        output.interleaved_samples[out_frame * output_format_.channels + out_channel] =
            left_sample + (right_sample - left_sample) * fraction;
      }
    }

    return output;
  }

  std::wstring mode_name() const override {
    return L"LinearFallback";
  }

 private:
  AudioFormatSpec input_format_ {};
  AudioFormatSpec output_format_ {};
  bool configured_ = false;
};

class MediaFoundationAudioResampler final : public IAudioResampler {
 public:
  MediaFoundationAudioResampler() {
    const auto hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    mf_started_ = SUCCEEDED(hr);
  }

  ~MediaFoundationAudioResampler() override {
    transform_.Reset();
    if (mf_started_) {
      MFShutdown();
    }
  }

  bool Configure(const AudioFormatSpec& input_format,
                 const AudioFormatSpec& output_format) override {
    input_format_ = input_format;
    output_format_ = output_format;
    input_format_.normalize();
    output_format_.normalize();

    if (!mf_started_) {
      return false;
    }

    transform_.Reset();
    const auto hr = CoCreateInstance(CLSID_CResamplerMediaObject, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&transform_));
    if (FAILED(hr) || !transform_) {
      return false;
    }

    ComPtr<IWMResamplerProps> props;
    if (SUCCEEDED(transform_.As(&props)) && props) {
      props->SetHalfFilterLength(30);
    }

    auto input_type = BuildMediaType(input_format_);
    auto output_type = BuildMediaType(output_format_);
    if (!input_type || !output_type) {
      transform_.Reset();
      return false;
    }

    if (FAILED(transform_->SetInputType(0, input_type.Get(), 0)) ||
        FAILED(transform_->SetOutputType(0, output_type.Get(), 0))) {
      transform_.Reset();
      return false;
    }

    configured_ = true;
    return true;
  }

  std::optional<AudioFrameChunk> Resample(
      const AudioFrameChunk& input) override {
    if (!configured_ || !transform_) {
      return std::nullopt;
    }

    auto sample = BuildInputSample(input);
    if (!sample) {
      return std::nullopt;
    }

    if (FAILED(transform_->ProcessInput(0, sample.Get(), 0))) {
      return std::nullopt;
    }

    MFT_OUTPUT_STREAM_INFO stream_info {};
    if (FAILED(transform_->GetOutputStreamInfo(0, &stream_info))) {
      return std::nullopt;
    }

    DWORD output_bytes = stream_info.cbSize;
    if (output_bytes == 0) {
      output_bytes = static_cast<DWORD>(
          std::max<uint32_t>(1, input.frame_count()) * output_format_.frame_bytes() *
          2);
    }

    ComPtr<IMFSample> output_sample;
    ComPtr<IMFMediaBuffer> output_buffer;
    if (FAILED(MFCreateSample(&output_sample)) ||
        FAILED(MFCreateMemoryBuffer(output_bytes, &output_buffer)) ||
        FAILED(output_sample->AddBuffer(output_buffer.Get()))) {
      return std::nullopt;
    }

    MFT_OUTPUT_DATA_BUFFER output_data {};
    output_data.dwStreamID = 0;
    output_data.pSample = output_sample.Get();
    DWORD status = 0;
    const auto hr = transform_->ProcessOutput(0, 1, &output_data, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return AudioFrameChunk{output_format_, {}, input.first_sample_qpc,
                             input.frame_index};
    }
    if (FAILED(hr)) {
      return std::nullopt;
    }

    DWORD total_length = 0;
    if (FAILED(output_buffer->GetCurrentLength(&total_length))) {
      return std::nullopt;
    }
    if (total_length == 0) {
      return AudioFrameChunk{output_format_, {}, input.first_sample_qpc,
                             input.frame_index};
    }

    BYTE* raw = nullptr;
    DWORD max_length = 0;
    if (FAILED(output_buffer->Lock(&raw, &max_length, &total_length)) || raw == nullptr) {
      return std::nullopt;
    }

    AudioFrameChunk output;
    output.format = output_format_;
    output.frame_index = input.frame_index;
    output.first_sample_qpc = input.first_sample_qpc;
    const auto frames = total_length / output_format_.frame_bytes();
    output.interleaved_samples = ConvertPcmToFloat(
        raw, frames, reinterpret_cast<const WAVEFORMATEX&>(
                         MakeWaveFormatExtensible(output_format_)));
    output_buffer->Unlock();
    return output;
  }

  std::wstring mode_name() const override {
    return L"MediaFoundation";
  }

 private:
  ComPtr<IMFMediaType> BuildMediaType(const AudioFormatSpec& format) {
    auto wave = MakeWaveFormatExtensible(format);
    ComPtr<IMFMediaType> media_type;
    if (FAILED(MFCreateMediaType(&media_type)) ||
        FAILED(MFInitMediaTypeFromWaveFormatEx(
            media_type.Get(), reinterpret_cast<WAVEFORMATEX*>(&wave),
            sizeof(WAVEFORMATEXTENSIBLE)))) {
      return nullptr;
    }
    return media_type;
  }

  ComPtr<IMFSample> BuildInputSample(const AudioFrameChunk& input) {
    std::vector<BYTE> bytes;
    auto wave = MakeWaveFormatExtensible(input_format_);
    ConvertFloatToPcm(input.interleaved_samples.data(), input.frame_count(),
                      reinterpret_cast<const WAVEFORMATEX&>(wave), &bytes);

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateSample(&sample)) ||
        FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer))) {
      return nullptr;
    }

    BYTE* dst = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    if (FAILED(buffer->Lock(&dst, &max_length, &current_length)) || dst == nullptr) {
      return nullptr;
    }
    std::memcpy(dst, bytes.data(), bytes.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
    sample->AddBuffer(buffer.Get());
    return sample;
  }

  AudioFormatSpec input_format_ {};
  AudioFormatSpec output_format_ {};
  ComPtr<IMFTransform> transform_;
  bool mf_started_ = false;
  bool configured_ = false;
};

class FallbackAudioResampler final : public IAudioResampler {
 public:
  FallbackAudioResampler()
      : primary_(std::make_unique<MediaFoundationAudioResampler>()),
        fallback_(std::make_unique<LinearAudioResampler>()) {}

  bool Configure(const AudioFormatSpec& input_format,
                 const AudioFormatSpec& output_format) override {
    input_format_ = input_format;
    output_format_ = output_format;
    using_primary_ = primary_->Configure(input_format, output_format);
    fallback_->Configure(input_format, output_format);
    return using_primary_ || true;
  }

  std::optional<AudioFrameChunk> Resample(
      const AudioFrameChunk& input) override {
    if (using_primary_) {
      const auto primary_output = primary_->Resample(input);
      if (primary_output.has_value() &&
          (primary_output->frame_count() > 0 || input.frame_count() == 0)) {
        return primary_output;
      }
      using_primary_ = false;
    }
    return fallback_->Resample(input);
  }

  std::wstring mode_name() const override {
    return using_primary_ ? primary_->mode_name() : fallback_->mode_name();
  }

 private:
  AudioFormatSpec input_format_ {};
  AudioFormatSpec output_format_ {};
  std::unique_ptr<IAudioResampler> primary_;
  std::unique_ptr<IAudioResampler> fallback_;
  bool using_primary_ = false;
};

}  // namespace

std::unique_ptr<IAudioResampler> CreateAudioResampler() {
  return std::make_unique<FallbackAudioResampler>();
}

}  // namespace winaudio
