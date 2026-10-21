#include "ChartDataPipeline.h"
#include "Analysis.h"
#include "Fft.h"
#include <algorithm>
#include <cstring>

namespace wa {
namespace {

bool fillLatestWaveMono(const ScopeReader& reader, float* wave, int waveN,
                        uint64_t* filledThrough) {
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
            uint64_t end = 0;
            if (!reader.snapshotLatest(static_cast<size_t>(delta), wave + keep, end))
                return false;
            *filledThrough = written;
            return true;
        }
    }

    const int head = waveN - static_cast<int>(nn);
    if (head > 0) std::fill(wave, wave + head, 0.f);
    uint64_t end = 0;
    if (!reader.snapshotLatest(nn, wave + head, end)) return false;
    if (filledThrough) *filledThrough = written;
    return true;
}

bool fillLatestWaveChannels(const ScopeReader& reader, std::vector<std::vector<float>>& channelWaves,
                            uint32_t channelCount, int waveN, uint64_t* filledThrough) {
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
            if (reader.snapshotChannelEndingAt(static_cast<uint16_t>(ch), written,
                                               static_cast<size_t>(delta),
                                               wave.data() + keep))
                any = true;
        }
    } else {
        for (uint32_t ch = 0; ch < shown; ++ch) {
            std::vector<float>& wave = channelWaves[static_cast<size_t>(ch)];
            if (static_cast<int>(wave.size()) < waveN) continue;
            const int head = waveN - static_cast<int>(nn);
            if (head > 0) std::fill(wave.begin(), wave.begin() + head, 0.f);
            if (reader.snapshotChannelEndingAt(static_cast<uint16_t>(ch), written, nn,
                                               wave.data() + head))
                any = true;
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
        if (!p.reader->snapshotEndingAt(endIdx, p.fftWin, b.specWin->data())) return;
        magnitudeSpectrumDb(b.specWin->data(), p.fftWin, b.work->data(), *b.mag);
        if (b.spectrogram) b.spectrogram->pushColumn(*b.mag);
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
            if (!p.reader->snapshotChannelEndingAt(
                    static_cast<uint16_t>(ch), endIdx, p.fftWin,
                    (*b.channelSpecWindows)[ch].data())) {
                allGot = false;
                break;
            }
        }
        if (!allGot) return;
        for (uint32_t ch = 0; ch < nCh; ++ch) {
            magnitudeSpectrumDb((*b.channelSpecWindows)[ch].data(), p.fftWin, b.work->data(),
                                *b.mag);
            if ((*b.channelSpecs)[ch])
                (*b.channelSpecs)[ch]->pushColumn(*b.mag);
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
            result.haveWave = fillLatestWaveChannels(*params.reader, *buffers.channelWaves,
                                                     buffers.channelCount, buffers.waveN,
                                                     buffers.waveFilledThrough);
        else if (buffers.wave)
            result.haveWave = fillLatestWaveMono(*params.reader, buffers.wave, buffers.waveN,
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