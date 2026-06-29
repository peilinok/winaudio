#include "AudioFormat.h"

namespace wa {

WAVEFORMATEXTENSIBLE toWaveFormatExtensible(const AudioFormat& f) {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = f.channels;
    w.Format.nSamplesPerSec = f.sampleRate;
    w.Format.wBitsPerSample = f.bitsPerSample;
    w.Format.nBlockAlign = static_cast<WORD>(f.blockAlign());
    w.Format.nAvgBytesPerSec = f.avgBytesPerSec();
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = f.bitsPerSample;
    // Standard speaker masks for common channel counts; 0 (unspecified) otherwise.
    switch (f.channels) {
        case 1: w.dwChannelMask = SPEAKER_FRONT_CENTER; break;
        case 2: w.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; break;
        case 4: w.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                                | SPEAKER_BACK_LEFT  | SPEAKER_BACK_RIGHT; break;
        case 6: w.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                                | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY
                                | SPEAKER_BACK_LEFT  | SPEAKER_BACK_RIGHT; break;
        case 8: w.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                                | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY
                                | SPEAKER_BACK_LEFT  | SPEAKER_BACK_RIGHT
                                | SPEAKER_SIDE_LEFT  | SPEAKER_SIDE_RIGHT; break;
        default: w.dwChannelMask = 0; break; // unspecified speaker assignment
    }
    w.SubFormat = f.isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                            : KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

AudioFormat fromWaveFormat(const WAVEFORMATEX* wf) {
    AudioFormat f{};
    f.sampleRate = wf->nSamplesPerSec;
    f.channels = wf->nChannels;
    f.bitsPerSample = wf->wBitsPerSample;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        f.isFloat = true;
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        f.isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        f.isFloat = false; // WAVE_FORMAT_PCM
    }
    return f;
}

} // namespace wa
