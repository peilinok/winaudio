#include <cmath>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <crtdbg.h>
#include <windows.h>
#include <vector>

#include "audio/audio_types.h"
#include "audio/pipeline/audio_ring_buffer.h"
#include "audio/pipeline/signal_analyzer.h"
#include "audio/pipeline/wav_dump_writer.h"

using namespace winaudio;

namespace {

bool NearlyEqual(float left, float right, float epsilon = 0.001f) {
  return std::fabs(left - right) <= epsilon;
}

AudioFormatSpec MakeFloatStereoFormat() {
  AudioFormatSpec format;
  format.sample_rate = 48000;
  format.channels = 2;
  format.sample_type = AudioSampleType::Float32;
  format.normalize();
  return format;
}

AudioFrameChunk MakeChunk(const AudioFormatSpec& format,
                          std::initializer_list<float> samples) {
  AudioFrameChunk chunk;
  chunk.format = format;
  chunk.interleaved_samples.assign(samples.begin(), samples.end());
  return chunk;
}

bool TestAudioFormatNormalization() {
  AudioFormatSpec format;
  format.sample_rate = 44100;
  format.channels = 1;
  format.sample_type = AudioSampleType::PcmInt16;
  format.normalize();
  return format.bits_per_sample == 16 && format.block_align == 2 &&
         format.avg_bytes_per_sec == 88200 &&
         DescribeAudioFormat(format) == L"44100 Hz / 1 ch / PCM16";
}

bool TestRingBufferPushPeekPop() {
  const auto format = MakeFloatStereoFormat();
  AudioRingBuffer ring(format, 8);
  ring.Push(MakeChunk(format, {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f}));

  const auto delayed = ring.PeekDelayedFrames(1, 2);
  if (!delayed.has_value() || delayed->frame_count() != 2) {
    return false;
  }
  if (!NearlyEqual(delayed->interleaved_samples[0], 0.2f) ||
      !NearlyEqual(delayed->interleaved_samples[3], -0.3f)) {
    return false;
  }

  const auto popped = ring.PopFrames(2);
  if (!popped.has_value() || popped->frame_count() != 2) {
    return false;
  }
  return NearlyEqual(popped->interleaved_samples[0], 0.1f) &&
         NearlyEqual(popped->interleaved_samples[3], -0.2f) &&
         ring.size_frames() == 2;
}

bool TestRingBufferDropsOldestFrames() {
  const auto format = MakeFloatStereoFormat();
  AudioRingBuffer ring(format, 3);
  ring.Push(MakeChunk(format, {0.1f, 0.1f, 0.2f, 0.2f, 0.3f, 0.3f}));
  ring.Push(MakeChunk(format, {0.4f, 0.4f, 0.5f, 0.5f}));
  const auto popped = ring.PopFrames(3);
  if (!popped.has_value()) {
    return false;
  }
  return ring.dropped_frames() == 2 &&
         NearlyEqual(popped->interleaved_samples[0], 0.3f) &&
         NearlyEqual(popped->interleaved_samples[4], 0.5f);
}

bool TestSignalAnalyzerMeterAndWaveform() {
  const auto format = MakeFloatStereoFormat();
  SignalAnalyzer analyzer(4);
  analyzer.Push(MakeChunk(format, {0.5f, -0.5f, 1.0f, -1.0f}));
  const auto meter = analyzer.meter();
  const auto waveform = analyzer.waveform();
  return NearlyEqual(meter.peak, 1.0f) && meter.clipping &&
         meter.dbfs > -0.1f && waveform.size() == 1 &&
         NearlyEqual(waveform[0].min_value, -1.0f) &&
         NearlyEqual(waveform[0].max_value, 1.0f);
}

bool TestWavDumpWriterWritesHeaderAndPayload() {
  const auto format = MakeFloatStereoFormat();
  const auto temp_dir = std::filesystem::temp_directory_path();
  const auto path = temp_dir / "winaudio-core-pipeline-test.wav";

  WavDumpWriter writer;
  if (!writer.Open(path, format, DumpFileType::Wav)) {
    return false;
  }
  const auto chunk = MakeChunk(format, {0.1f, -0.1f, 0.2f, -0.2f});
  if (!writer.Write(chunk)) {
    writer.Close();
    return false;
  }
  writer.Close();

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return false;
  }
  const auto size = file.tellg();
  file.seekg(0, std::ios::beg);
  char riff[4] = {};
  file.read(riff, 4);
  file.close();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return std::string(riff, 4) == "RIFF" &&
         size == static_cast<std::streamoff>(44 + chunk.interleaved_samples.size() * sizeof(float)) &&
         !ec;
}

}  // namespace

int main() {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

  struct NamedTest {
    const char* name;
    bool (*fn)();
  };

  const std::vector<NamedTest> tests = {
      {"AudioFormatNormalization", &TestAudioFormatNormalization},
      {"RingBufferPushPeekPop", &TestRingBufferPushPeekPop},
      {"RingBufferDropsOldestFrames", &TestRingBufferDropsOldestFrames},
      {"SignalAnalyzerMeterAndWaveform", &TestSignalAnalyzerMeterAndWaveform},
      {"WavDumpWriterWritesHeaderAndPayload", &TestWavDumpWriterWritesHeaderAndPayload},
  };

  for (const auto& test : tests) {
    std::cerr << "RUNNING: " << test.name << "\n";
    try {
      if (!test.fn()) {
        std::cerr << "FAILED: " << test.name << "\n";
        return 1;
      }
    } catch (const std::exception& ex) {
      std::cerr << "EXCEPTION: " << test.name << ": " << ex.what() << "\n";
      return 2;
    } catch (...) {
      std::cerr << "EXCEPTION: " << test.name << ": unknown\n";
      return 3;
    }
  }

  std::cerr << "ALL_TESTS_PASSED\n";
  return 0;
}
