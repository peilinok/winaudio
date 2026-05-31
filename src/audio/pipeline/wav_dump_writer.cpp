#include "wav_dump_writer.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace winaudio {

namespace {

template <typename T>
void WriteValue(std::ofstream& file, T value) {
  file.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

int16_t FloatToPcm16(float sample) {
  const auto clamped = std::clamp(sample, -1.0f, 1.0f);
  return static_cast<int16_t>(clamped * 32767.0f);
}

int32_t FloatToPcm24Packed(float sample) {
  const auto clamped = std::clamp(sample, -1.0f, 1.0f);
  return static_cast<int32_t>(clamped * 8388607.0f);
}

int32_t FloatToPcm32(float sample) {
  const auto clamped = std::clamp(sample, -1.0f, 1.0f);
  return static_cast<int32_t>(clamped * 2147483647.0f);
}

}  // namespace

WavDumpWriter::~WavDumpWriter() {
  Close();
}

bool WavDumpWriter::Open(const std::filesystem::path& path,
                         const AudioFormatSpec& format,
                         DumpFileType file_type) {
  Close();
  format_ = format;
  format_.normalize();
  file_type_ = file_type;

  file_.open(path, std::ios::binary | std::ios::trunc);
  if (!file_.is_open()) {
    return false;
  }
  written_frames_ = 0;
  if (file_type_ == DumpFileType::Wav) {
    WriteWavHeaderPlaceholder();
  }
  return true;
}

bool WavDumpWriter::Write(const AudioFrameChunk& chunk) {
  if (!file_.is_open()) {
    return false;
  }
  for (float sample : chunk.interleaved_samples) {
    WriteRawSample(sample);
  }
  written_frames_ += chunk.frame_count();
  return true;
}

void WavDumpWriter::Close() {
  if (!file_.is_open()) {
    return;
  }
  if (file_type_ == DumpFileType::Wav) {
    FinalizeWavHeader();
  }
  file_.flush();
  file_.close();
}

bool WavDumpWriter::is_open() const {
  return file_.is_open();
}

uint64_t WavDumpWriter::written_frames() const {
  return written_frames_;
}

void WavDumpWriter::WriteWavHeaderPlaceholder() {
  file_.write("RIFF", 4);
  WriteValue<uint32_t>(file_, 0);
  file_.write("WAVE", 4);
  file_.write("fmt ", 4);
  WriteValue<uint32_t>(file_, 16);
  const uint16_t format_tag =
      format_.sample_type == AudioSampleType::Float32 ? 3 : 1;
  WriteValue<uint16_t>(file_, format_tag);
  WriteValue<uint16_t>(file_, format_.channels);
  WriteValue<uint32_t>(file_, format_.sample_rate);
  WriteValue<uint32_t>(file_, format_.avg_bytes_per_sec);
  WriteValue<uint16_t>(file_, format_.block_align);
  WriteValue<uint16_t>(file_, format_.bits_per_sample);
  file_.write("data", 4);
  WriteValue<uint32_t>(file_, 0);
}

void WavDumpWriter::FinalizeWavHeader() {
  const auto data_bytes = static_cast<uint32_t>(written_frames_ * format_.block_align);
  const auto riff_size = 36u + data_bytes;

  file_.seekp(4, std::ios::beg);
  WriteValue<uint32_t>(file_, riff_size);
  file_.seekp(40, std::ios::beg);
  WriteValue<uint32_t>(file_, data_bytes);
  file_.seekp(0, std::ios::end);
}

void WavDumpWriter::WriteRawSample(float sample) {
  switch (format_.sample_type) {
    case AudioSampleType::PcmInt16: {
      auto value = FloatToPcm16(sample);
      WriteValue<int16_t>(file_, value);
      break;
    }
    case AudioSampleType::PcmInt24: {
      const auto value = FloatToPcm24Packed(sample);
      const std::array<char, 3> bytes = {
          static_cast<char>(value & 0xFF),
          static_cast<char>((value >> 8) & 0xFF),
          static_cast<char>((value >> 16) & 0xFF),
      };
      file_.write(bytes.data(), bytes.size());
      break;
    }
    case AudioSampleType::PcmInt32: {
      auto value = FloatToPcm32(sample);
      WriteValue<int32_t>(file_, value);
      break;
    }
    case AudioSampleType::Float32: {
      WriteValue<float>(file_, sample);
      break;
    }
  }
}

}  // namespace winaudio
