#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "audio/audio_types.h"

namespace winaudio {

class AudioRingBuffer {
 public:
  AudioRingBuffer(const AudioFormatSpec& format, uint32_t capacity_frames);

  void Push(const AudioFrameChunk& chunk);
  std::optional<AudioFrameChunk> PopFrames(uint32_t frames);
  std::optional<AudioFrameChunk> PeekDelayedFrames(uint32_t delay_frames,
                                                   uint32_t frames) const;
  void Clear();

  [[nodiscard]] uint32_t capacity_frames() const;
  [[nodiscard]] uint64_t size_frames() const;
  [[nodiscard]] uint64_t dropped_frames() const;

 private:
  [[nodiscard]] AudioFrameChunk BuildChunkLocked(uint64_t start_frame,
                                                 uint32_t frames) const;

  AudioFormatSpec format_;
  uint32_t capacity_frames_;
  mutable std::mutex mutex_;
  std::deque<float> samples_;
  uint64_t head_frame_ = 0;
  uint64_t dropped_frames_ = 0;
};

}  // namespace winaudio
