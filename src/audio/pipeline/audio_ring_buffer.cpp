#include "audio_ring_buffer.h"

#include <algorithm>

namespace winaudio {

AudioRingBuffer::AudioRingBuffer(const AudioFormatSpec& format,
                                 uint32_t capacity_frames)
    : format_(format), capacity_frames_(capacity_frames) {}

void AudioRingBuffer::Push(const AudioFrameChunk& chunk) {
  std::scoped_lock lock(mutex_);
  const auto frame_count = chunk.frame_count();
  const auto needed_frames =
      static_cast<uint64_t>(size_frames()) + frame_count;
  if (needed_frames > capacity_frames_) {
    const auto frames_to_drop =
        std::min<uint64_t>(needed_frames - capacity_frames_, size_frames());
    const auto samples_to_drop = frames_to_drop * format_.channels;
    for (uint64_t index = 0; index < samples_to_drop; ++index) {
      samples_.pop_front();
    }
    head_frame_ += frames_to_drop;
    dropped_frames_ += frames_to_drop;
  }
  for (float sample : chunk.interleaved_samples) {
    samples_.push_back(sample);
  }
}

std::optional<AudioFrameChunk> AudioRingBuffer::PopFrames(uint32_t frames) {
  std::scoped_lock lock(mutex_);
  if (size_frames() < frames) {
    return std::nullopt;
  }

  auto chunk = BuildChunkLocked(head_frame_, frames);
  const auto samples_to_pop = static_cast<uint64_t>(frames) * format_.channels;
  for (uint64_t index = 0; index < samples_to_pop; ++index) {
    samples_.pop_front();
  }
  head_frame_ += frames;
  return chunk;
}

std::optional<AudioFrameChunk> AudioRingBuffer::PeekDelayedFrames(
    uint32_t delay_frames,
    uint32_t frames) const {
  std::scoped_lock lock(mutex_);
  const auto available = size_frames();
  if (available < static_cast<uint64_t>(delay_frames) + frames) {
    return std::nullopt;
  }

  const auto start_frame = head_frame_ + available - delay_frames - frames;
  return BuildChunkLocked(start_frame, frames);
}

void AudioRingBuffer::Clear() {
  std::scoped_lock lock(mutex_);
  samples_.clear();
  head_frame_ = 0;
  dropped_frames_ = 0;
}

uint32_t AudioRingBuffer::capacity_frames() const {
  return capacity_frames_;
}

uint64_t AudioRingBuffer::size_frames() const {
  return samples_.size() / format_.channels;
}

uint64_t AudioRingBuffer::dropped_frames() const {
  std::scoped_lock lock(mutex_);
  return dropped_frames_;
}

AudioFrameChunk AudioRingBuffer::BuildChunkLocked(uint64_t start_frame,
                                                  uint32_t frames) const {
  AudioFrameChunk chunk;
  chunk.format = format_;
  chunk.frame_index = start_frame;
  chunk.interleaved_samples.resize(static_cast<size_t>(frames) * format_.channels);
  const auto start_offset_frames = start_frame - head_frame_;
  const auto start_sample = start_offset_frames * format_.channels;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    for (uint16_t channel = 0; channel < format_.channels; ++channel) {
      const auto source_index =
          static_cast<size_t>(start_sample + frame * format_.channels + channel);
      const auto target_index =
          static_cast<size_t>(frame * format_.channels + channel);
      chunk.interleaved_samples[target_index] = samples_[source_index];
    }
  }
  return chunk;
}

}  // namespace winaudio
