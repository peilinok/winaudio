#include "AudioFormat.h"

namespace wa {

WAVEFORMATEXTENSIBLE AudioFormat::toWaveFormatExtensible() const {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = channels;
    w.Format.nSamplesPerSec = sampleRate;
    w.Format.wBitsPerSample = bitsPerSample;
    w.Format.nBlockAlign = static_cast<WORD>(blockAlign());
    w.Format.nAvgBytesPerSec = avgBytesPerSec();
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = bitsPerSample;
    w.dwChannelMask = (channels == 1) ? SPEAKER_FRONT_CENTER
                                      : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    w.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                          : KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

AudioFormat AudioFormat::fromWaveFormat(const WAVEFORMATEX* wf) {
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
