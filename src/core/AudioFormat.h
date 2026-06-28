#pragma once
#include <cstdint>
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

namespace wa {

struct AudioFormat {
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    uint16_t bitsPerSample = 16;
    bool     isFloat = false;

    uint32_t blockAlign() const {
        return static_cast<uint32_t>(channels) * (bitsPerSample / 8u);
    }
    uint32_t avgBytesPerSec() const { return sampleRate * blockAlign(); }

    WAVEFORMATEXTENSIBLE toWaveFormatExtensible() const;
    static AudioFormat fromWaveFormat(const WAVEFORMATEX* wf);

    bool operator==(const AudioFormat& o) const {
        return sampleRate == o.sampleRate && channels == o.channels &&
               bitsPerSample == o.bitsPerSample && isFloat == o.isFloat;
    }
};

} // namespace wa
