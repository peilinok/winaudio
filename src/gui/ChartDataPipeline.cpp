#include "ChartDataPipeline.h"
#include "Analysis.h"
#include "Fft.h"
#include "FrameProbe.h"
#include <algorithm>
#include <cstring>

namespace wa {
namespace {

void note(const ChartRefreshParams& p, const char* metric, uint64_t value, int channel) {
    if (p.probe) p.probe->addWork(metric, value, p.source, p.direction, channel);
}

bool fillLatestWaveMono(const ChartRefreshParams& p, float* wave, int waveN,
                        uint64_t* filledThrough) {
    const ScopeReader& reader = *p.reader;
    if (!wave || waveN <= 0) return false;
    const uint64_t written = reader.written();
    if (written == 0) return false;

    if (filledThrough && *filledThrough == written) return true;

    const size_t nn = static_cast<size_t>(std::min<uint64_t>(written, static_cast<uint64_t>(waveN)));
    if (nn == 0) return false;

    if (filledThrough && *filledThrough > 0 && written > *filledThrough) {
        const uint64_t delta64 = written - *filledThrough;
        if (delta64 < static_cast<uint64_t>(waveN) &&
            static_cast<uint64_t>(waveN) <= written) {
            // Steady-state incremental: shift left, snapshot only the new tail.
            const int delta = static_cast<int>(delta64);
            const int keep = waveN - delta;
            std::memmove(wave, wave + delta, static_cast<size_t>(keep) * sizeof(float));
            note(p, "wave-move-bytes", static_cast<uint64_t>(keep) * sizeof(float), 0);
            uint64_t end = 0;
            note(p, "wave-request-samples", static_cast<uint64_t>(delta), 0);
            if (!reader.snapshotLatest(static_cast<size_t>(delta), wave + keep, end))
                return false;
            note(p, "wave-copy-bytes", static_cast<uint64_t>(delta) * sizeof(float), 0);
            *filledThrough = written;
            return true;
        }
    }

    const int head = waveN - static_cast<int>(nn);
    if (head > 0) std::fill(wave, wave + head, 0.f);
    uint64_t end = 0;
    note(p, "wave-request-samples", nn, 0);
    if (!reader.snapshotLatest(nn, wave + head, end)) return false;
    note(p, "wave-copy-bytes", nn * sizeof(float), 0);
    note(p, "wave-zero-bytes", static_cast<uint64_t>(head) * sizeof(float), 0);
    if (filledThrough) *filledThrough = written;
    return true;
}

bool fillLatestWaveChannels(const ChartRefreshParams& p, std::vector<std::vector<float>>& channelWaves,
                            uint32_t channelCount, int waveN, uint64_t* filledThrough) {
    const ScopeReader& reader = *p.reader;
    if (waveN <= 0 || channelCount == 0) return false;
    const uint64_t written = reader.written();
    if (written == 0) return false;

    if (filledThrough && *filledThrough == written) {
        const uint32_t shown =
            std::min(channelCount, static_cast<uint32_t>(channelWaves.size()));
        return shown > 0 && static_cast<int>(channelWaves[0].size()) >= waveN;
    }

    const size_t nn = static_cast<size_t>(std::min<uint64_t>(written, static_cast<uint64_t>(waveN)));
    if (nn == 0) return false;
    const uint32_t shown =
        std::min(channelCount, static_cast<uint32_t>(channelWaves.size()));
    if (shown == 0) return false;

    const bool canIncremental = filledThrough && *filledThrough > 0 && written > *filledThrough &&
                                static_cast<uint64_t>(waveN) <= written;
    const uint64_t delta64 = canIncremental ? (written - *filledThrough) : 0;
    const bool incremental =
        canIncremental && delta64 < static_cast<uint64_t>(waveN);

    bool any = false;
    if (incremental) {
        const int delta = static_cast<int>(delta64);
        const int keep = waveN - delta;
        for (uint32_t ch = 0; ch < shown; ++ch) {
            std::vector<float>& wave = channelWaves[static_cast<size_t>(ch)];
            if (static_cast<int>(wave.size()) < waveN) continue;
            std::memmove(wave.data(), wave.data() + delta,
                         static_cast<size_t>(keep) * sizeof(float));
            note(p, "wave-move-bytes", static_cast<uint64_t>(keep) * sizeof(float), static_cast<int>(ch));
            note(p, "wave-request-samples", static_cast<uint64_t>(delta), static_cast<int>(ch));
            if (reader.snapshotChannelEndingAt(static_cast<uint16_t>(ch), written,
                                               static_cast<size_t>(delta),
                                               wave.data() + keep)) {
                note(p, "wave-copy-bytes", static_cast<uint64_t>(delta) * sizeof(float), static_cast<int>(ch));
                any = true;
            }
        }
    } else {
        for (uint32_t ch = 0; ch < shown; ++ch) {
            std::vector<float>& wave = channelWaves[static_cast<size_t>(ch)];
            if (static_cast<int>(wave.size()) < waveN) continue;
            const int head = waveN - static_cast<int>(nn);
            if (head > 0) std::fill(wave.begin(), wave.begin() + head, 0.f);
            note(p, "wave-zero-bytes", static_cast<uint64_t>(head) * sizeof(float), static_cast<int>(ch));
            note(p, "wave-request-samples", nn, static_cast<int>(ch));
            if (reader.snapshotChannelEndingAt(static_cast<uint16_t>(ch), written, nn,
                                               wave.data() + head)) {
                note(p, "wave-copy-bytes", nn * sizeof(float), static_cast<int>(ch));
                any = true;
            }
        }
    }
    if (any && filledThrough) *filledThrough = written;
    return any;
}

void advanceSpectraMono(const ChartRefreshParams& p, ChartBuffers& b) {
    if (!p.reader || !b.nextEnd || !b.specWin || !b.work || !b.mag) return;
    if (b.specWin->size() < p.fftWin || b.work->size() < p.fftWin) return;
    const uint64_t written = p.reader->written();
    advanceAnalysis(written, *b.nextEnd, p.fftWin, p.fftHop, p.maxCatchup, [&](uint64_t endIdx) {
        note(p, "fft-request-samples", p.fftWin, 0);
        if (!p.reader->snapshotEndingAt(endIdx, p.fftWin, b.specWin->data())) return;
        {
            FrameBracket fft(p.probe, FrameSlice::Fft);
            magnitudeSpectrumDb(b.specWin->data(), p.fftWin, b.work->data(), *b.mag);
        }
        if (b.spectrogram) {
            FrameBracket history(p.probe, FrameSlice::SpecHistory);
            b.spectrogram->pushColumn(*b.mag);
            note(p, "spec-hops", 1, 0);
            note(p, "spec-move-bytes", static_cast<uint64_t>(b.spectrogram->rows()) *
                static_cast<uint64_t>(b.spectrogram->cols() - 1) * sizeof(float), 0);
        }
    });
}

void advanceSpectraMulti(const ChartRefreshParams& p, ChartBuffers& b) {
    if (!p.reader || !b.nextEnd || !b.channelSpecWindows || !b.channelSpecs || !b.work || !b.mag)
        return;
    if (b.work->size() < p.fftWin) return;
    const uint32_t nCh = std::min(
        b.channelCount,
        std::min(static_cast<uint32_t>(b.channelSpecWindows->size()),
                 static_cast<uint32_t>(b.channelSpecs->size())));
    if (nCh == 0) return;
    for (uint32_t ch = 0; ch < nCh; ++ch) {
        if ((*b.channelSpecWindows)[ch].size() < p.fftWin) return;
    }
    const uint64_t written = p.reader->written();
    advanceAnalysis(written, *b.nextEnd, p.fftWin, p.fftHop, p.maxCatchup, [&](uint64_t endIdx) {
        bool allGot = true;
        for (uint32_t ch = 0; ch < nCh; ++ch) {
            note(p, "fft-request-samples", p.fftWin, static_cast<int>(ch));
            if (!p.reader->snapshotChannelEndingAt(
                    static_cast<uint16_t>(ch), endIdx, p.fftWin,
                    (*b.channelSpecWindows)[ch].data())) {
                allGot = false;
                break;
            }
        }
        if (!allGot) return;
        for (uint32_t ch = 0; ch < nCh; ++ch) {
            {
                FrameBracket fft(p.probe, FrameSlice::Fft);
                magnitudeSpectrumDb((*b.channelSpecWindows)[ch].data(), p.fftWin, b.work->data(),
                                    *b.mag);
            }
            if ((*b.channelSpecs)[ch]) {
                FrameBracket history(p.probe, FrameSlice::SpecHistory);
                (*b.channelSpecs)[ch]->pushColumn(*b.mag);
                note(p, "spec-hops", 1, static_cast<int>(ch));
                note(p, "spec-move-bytes", static_cast<uint64_t>((*b.channelSpecs)[ch]->rows()) *
                    static_cast<uint64_t>((*b.channelSpecs)[ch]->cols() - 1) * sizeof(float), static_cast<int>(ch));
            }
        }
    });
}

bool buffersHaveWave(const ChartBuffers& b) {
    if (b.waveN <= 0 || b.waveSr == 0) return false;
    if (b.channelCount >= 2 && b.channelWaves)
        return !b.channelWaves->empty() && !(*b.channelWaves)[0].empty();
    return b.wave != nullptr;
}

} // namespace

ChartRefreshResult refreshCharts(const ChartRefreshParams& params, ChartBuffers& buffers) {
    FrameBracket refresh(params.probe, FrameSlice::ChartRefresh);
    ChartRefreshResult result;
    if (!params.reader) return result;

    if (params.frozen) {
        result.haveWave = buffersHaveWave(buffers);
        return result;
    }
    if (!params.streamActive) return result;

    const bool multi = buffers.channelCount >= 2 && buffers.channelWaves != nullptr;

    if (buffers.waveSr > 0 && buffers.waveN > 0) {
        if (multi)
            result.haveWave = fillLatestWaveChannels(params, *buffers.channelWaves,
                                                     buffers.channelCount, buffers.waveN,
                                                     buffers.waveFilledThrough);
        else if (buffers.wave)
            result.haveWave = fillLatestWaveMono(params, buffers.wave, buffers.waveN,
                                                 buffers.waveFilledThrough);
    }

    if (buffers.specSr > 0 && buffers.nextEnd) {
        if (multi)
            advanceSpectraMulti(params, buffers);
        else
            advanceSpectraMono(params, buffers);
    }

    return result;
}

} // namespace wa
