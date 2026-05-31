#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>

#include "audio/audio_types.h"

namespace winaudio {

class WavDumpWriter {
 public:
  WavDumpWriter() = default;
  ~WavDumpWriter();

  bool Open(const std::filesystem::path& path,
            const AudioFormatSpec& format,
            DumpFileType file_type);
  bool Write(const AudioFrameChunk& chunk);
  void Close();

  [[nodiscard]] bool is_open() const;
  [[nodiscard]] uint64_t written_frames() const;

 private:
  void WriteWavHeaderPlaceholder();
  void FinalizeWavHeader();
  void WriteRawSample(float sample);

  std::ofstream file_;
  AudioFormatSpec format_ {};
  DumpFileType file_type_ = DumpFileType::Wav;
  uint64_t written_frames_ = 0;
};

}  // namespace winaudio
