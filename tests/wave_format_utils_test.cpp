#include <cstring>
#include <iostream>
#include <vector>

#include "audio/backends/wave_format_utils.h"

using namespace winaudio;

namespace {

bool NearlyEqual(float left, float right, float epsilon = 0.001f) {
  return std::fabs(left - right) <= epsilon;
}

bool TestWaveFormatRoundTripFloatStereo() {
  AudioFormatSpec format;
  format.sample_rate = 48000;
  format.channels = 2;
  format.sample_type = AudioSampleType::Float32;
  format.normalize();

  const auto wave = MakeWaveFormatExtensible(format);
  const auto round_trip = AudioFormatFromWaveFormat(
      reinterpret_cast<const WAVEFORMATEX&>(wave));
  return wave.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         wave.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT &&
         round_trip.sample_rate == format.sample_rate &&
         round_trip.channels == format.channels &&
         round_trip.sample_type == AudioSampleType::Float32;
}

bool TestInt16PcmConversionToFloat() {
  WAVEFORMATEX format {};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 1;
  format.nSamplesPerSec = 16000;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 2;
  format.nAvgBytesPerSec = 32000;

  const int16_t samples[] = {-32767, 0, 16384, 32767};
  const auto converted = ConvertPcmToFloat(
      reinterpret_cast<const BYTE*>(samples), 4, format);
  return converted.size() == 4 && converted[0] < -0.99f &&
         NearlyEqual(converted[1], 0.0f) &&
         NearlyEqual(converted[2], 16384.0f / 32767.0f, 0.01f) &&
         converted[3] > 0.99f;
}

bool TestFloatConversionBackToPcm16() {
  WAVEFORMATEX format {};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 2;
  format.nSamplesPerSec = 48000;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 4;
  format.nAvgBytesPerSec = 192000;

  const float samples[] = {-1.0f, -0.5f, 0.0f, 1.0f};
  std::vector<BYTE> output;
  ConvertFloatToPcm(samples, 2, format, &output);
  if (output.size() != 8) {
    return false;
  }

  int16_t decoded[4] = {};
  std::memcpy(decoded, output.data(), output.size());
  return decoded[0] < -32000 && decoded[1] < -15000 && decoded[2] == 0 &&
         decoded[3] > 32000;
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    bool (*fn)();
  };

  const std::vector<NamedTest> tests = {
      {"WaveFormatRoundTripFloatStereo", &TestWaveFormatRoundTripFloatStereo},
      {"Int16PcmConversionToFloat", &TestInt16PcmConversionToFloat},
      {"FloatConversionBackToPcm16", &TestFloatConversionBackToPcm16},
  };

  for (const auto& test : tests) {
    std::cerr << "RUNNING: " << test.name << "\n";
    if (!test.fn()) {
      std::cerr << "FAILED: " << test.name << "\n";
      return 1;
    }
  }

  std::cerr << "ALL_TESTS_PASSED\n";
  return 0;
}
