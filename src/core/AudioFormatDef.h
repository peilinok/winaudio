#pragma once
#include <cstdint>
namespace wa {
struct AudioFormat {
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    uint16_t bitsPerSample = 16;
    bool     isFloat = false;
    uint16_t validBitsPerSample = 0; // 0 means bitsPerSample
    uint32_t channelMask = 0;        // 0 means unspecified
    uint32_t blockAlign() const { return static_cast<uint32_t>(channels) * (bitsPerSample / 8u); }
    uint32_t avgBytesPerSec() const { return sampleRate * blockAlign(); }
    uint16_t validBits() const {
        return validBitsPerSample ? validBitsPerSample : bitsPerSample;
    }
    bool operator==(const AudioFormat& o) const {
        return sampleRate == o.sampleRate && channels == o.channels &&
               bitsPerSample == o.bitsPerSample && isFloat == o.isFloat &&
               validBits() == o.validBits();
    }
};
} // namespace wa
