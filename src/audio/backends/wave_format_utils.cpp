#include "wave_format_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace winaudio {

namespace {

AudioSampleType SampleTypeFromSubFormat(const GUID& sub_format,
                                        uint16_t bits_per_sample) {
  if (sub_format == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
    return AudioSampleType::Float32;
  }

  switch (bits_per_sample) {
    case 16:
      return AudioSampleType::PcmInt16;
    case 24:
      return AudioSampleType::PcmInt24;
    default:
      return AudioSampleType::PcmInt32;
  }
}

float Int16ToFloat(int16_t sample) {
  return static_cast<float>(sample) / 32767.0f;
}

float Int24ToFloat(const BYTE* sample_bytes) {
  int32_t value = sample_bytes[0] | (sample_bytes[1] << 8) |
                  (sample_bytes[2] << 16);
  if ((value & 0x00800000) != 0) {
    value |= 0xFF000000;
  }
  return static_cast<float>(value) / 8388607.0f;
}

float Int32ToFloat(int32_t sample) {
  return static_cast<float>(sample) / 2147483647.0f;
}

int16_t FloatToInt16(float sample) {
  return static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
}

int32_t FloatToInt24(float sample) {
  return static_cast<int32_t>(std::clamp(sample, -1.0f, 1.0f) * 8388607.0f);
}

int32_t FloatToInt32(float sample) {
  return static_cast<int32_t>(std::clamp(sample, -1.0f, 1.0f) * 2147483647.0f);
}

}  // namespace

WAVEFORMATEXTENSIBLE MakeWaveFormatExtensible(const AudioFormatSpec& format) {
  AudioFormatSpec normalized = format;
  normalized.normalize();

  WAVEFORMATEXTENSIBLE wave {};
  wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wave.Format.nChannels = normalized.channels;
  wave.Format.nSamplesPerSec = normalized.sample_rate;
  wave.Format.wBitsPerSample = normalized.bits_per_sample;
  wave.Format.nBlockAlign = normalized.block_align;
  wave.Format.nAvgBytesPerSec = normalized.avg_bytes_per_sec;
  wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wave.Samples.wValidBitsPerSample = normalized.bits_per_sample;
  wave.dwChannelMask = normalized.channel_mask;
  wave.SubFormat = normalized.sample_type == AudioSampleType::Float32
                       ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                       : KSDATAFORMAT_SUBTYPE_PCM;
  return wave;
}

AudioFormatSpec AudioFormatFromWaveFormat(const WAVEFORMATEX& format) {
  AudioFormatSpec spec;
  spec.sample_rate = format.nSamplesPerSec;
  spec.channels = format.nChannels;
  spec.bits_per_sample = format.wBitsPerSample;
  spec.block_align = format.nBlockAlign;
  spec.avg_bytes_per_sec = format.nAvgBytesPerSec;

  if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    spec.sample_type = AudioSampleType::Float32;
  } else if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto& extensible =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
    spec.channel_mask = extensible.dwChannelMask;
    spec.sample_type =
        SampleTypeFromSubFormat(extensible.SubFormat, format.wBitsPerSample);
  } else {
    switch (format.wBitsPerSample) {
      case 16:
        spec.sample_type = AudioSampleType::PcmInt16;
        break;
      case 24:
        spec.sample_type = AudioSampleType::PcmInt24;
        break;
      case 32:
        spec.sample_type = AudioSampleType::PcmInt32;
        break;
      default:
        spec.sample_type = AudioSampleType::PcmInt16;
        break;
    }
  }

  spec.normalize();
  return spec;
}

std::vector<float> ConvertPcmToFloat(const BYTE* data,
                                     uint32_t frames,
                                     const WAVEFORMATEX& format) {
  const auto spec = AudioFormatFromWaveFormat(format);
  std::vector<float> output;
  output.resize(static_cast<size_t>(frames) * spec.channels);

  for (uint32_t frame = 0; frame < frames; ++frame) {
    const auto* frame_ptr = data + static_cast<size_t>(frame) * format.nBlockAlign;
    for (uint16_t channel = 0; channel < spec.channels; ++channel) {
      const auto* sample_ptr = frame_ptr + channel * (format.wBitsPerSample / 8);
      float sample = 0.0f;
      switch (spec.sample_type) {
        case AudioSampleType::PcmInt16:
          sample = Int16ToFloat(*reinterpret_cast<const int16_t*>(sample_ptr));
          break;
        case AudioSampleType::PcmInt24:
          sample = Int24ToFloat(sample_ptr);
          break;
        case AudioSampleType::PcmInt32:
          sample = Int32ToFloat(*reinterpret_cast<const int32_t*>(sample_ptr));
          break;
        case AudioSampleType::Float32:
          sample = *reinterpret_cast<const float*>(sample_ptr);
          break;
      }
      output[frame * spec.channels + channel] = sample;
    }
  }

  return output;
}

void ConvertFloatToPcm(const float* samples,
                       uint32_t frames,
                       const WAVEFORMATEX& format,
                       std::vector<BYTE>* output) {
  const auto spec = AudioFormatFromWaveFormat(format);
  output->assign(static_cast<size_t>(frames) * format.nBlockAlign, 0);

  for (uint32_t frame = 0; frame < frames; ++frame) {
    auto* frame_ptr = output->data() + static_cast<size_t>(frame) * format.nBlockAlign;
    for (uint16_t channel = 0; channel < spec.channels; ++channel) {
      auto* sample_ptr = frame_ptr + channel * (format.wBitsPerSample / 8);
      const auto sample = samples[frame * spec.channels + channel];
      switch (spec.sample_type) {
        case AudioSampleType::PcmInt16: {
          const auto value = FloatToInt16(sample);
          std::memcpy(sample_ptr, &value, sizeof(value));
          break;
        }
        case AudioSampleType::PcmInt24: {
          const auto value = FloatToInt24(sample);
          sample_ptr[0] = static_cast<BYTE>(value & 0xFF);
          sample_ptr[1] = static_cast<BYTE>((value >> 8) & 0xFF);
          sample_ptr[2] = static_cast<BYTE>((value >> 16) & 0xFF);
          break;
        }
        case AudioSampleType::PcmInt32: {
          const auto value = FloatToInt32(sample);
          std::memcpy(sample_ptr, &value, sizeof(value));
          break;
        }
        case AudioSampleType::Float32:
          std::memcpy(sample_ptr, &sample, sizeof(sample));
          break;
      }
    }
  }
}

}  // namespace winaudio
