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

bool waveBuffersValid(const ChartBuffers& b) {
    if (b.waveN <= 0 || b.waveSr == 0) return false;
    if (b.channelCount >= 2) {
        if (!b.channelWaves || b.channelWaves->size() < b.channelCount) return false;
        for (uint32_t ch = 0; ch < b.channelCount; ++ch)
            if ((*b.channelWaves)[ch].size() < static_cast<size_t>(b.waveN)) return false;
        return true;
    }
    return b.wave != nullptr;
}

ChartRefreshResult refreshWave(const ChartRefreshParams& p, ChartBuffers& b, uint64_t written) {
    ChartRefreshResult result;
    if (!b.waveState || !b.waveScratch) return result;
    auto& state = *b.waveState;
    if (!waveBuffersValid(b)) { state.valid = false; return result; }
    if (state.valid && written < state.filledThrough) {
        // Counter rollback invalidates the window, not the commit revision.
        state.valid = false;
        state.filledThrough = 0;
    }
    result.haveWave = state.valid;
    if (state.valid && written == state.filledThrough) {
        note(p, "wave-reuse", 1, -1);
        return result;
    }
    if (written == 0) return result;
    const bool multi = b.channelCount >= 2;
    const uint32_t channels = multi ? b.channelCount : 1;
    const uint64_t delta = state.valid ? written - state.filledThrough : 0;
    const bool incremental = state.valid && delta > 0 && delta < static_cast<uint64_t>(b.waveN);
    const size_t count = incremental ? static_cast<size_t>(delta)
        : static_cast<size_t>(std::min<uint64_t>(written, static_cast<uint64_t>(b.waveN)));
    auto& scratch = b.waveScratch->samples;
    if (scratch.capacity() < count * channels) {
        scratch.reserve(count * channels);
        note(p, "wave-scratch-allocations", 1, -1);
    }
    scratch.resize(count * channels);
    note(p, "wave-scratch-capacity-bytes", scratch.capacity() * sizeof(float), -1);
    for (uint32_t ch = 0; ch < channels; ++ch) {
        float* target = scratch.data() + static_cast<size_t>(ch) * count;
        note(p, "wave-request-samples", count, static_cast<int>(ch));
        const bool ok = multi
            ? p.reader->snapshotChannelEndingAt(static_cast<uint16_t>(ch), written, count, target)
            : p.reader->snapshotEndingAt(written, count, target);
        if (!ok) { note(p, "wave-failed", 1, static_cast<int>(ch)); return result; }
        note(p, "wave-snapshot-bytes", count * sizeof(float), static_cast<int>(ch));
    }
    // All reads succeeded. Only this commit phase may touch displayed history.
    const size_t head = static_cast<size_t>(b.waveN) - count;
    for (uint32_t ch = 0; ch < channels; ++ch) {
        float* wave = multi ? (*b.channelWaves)[ch].data() : b.wave;
        if (incremental) {
            std::memmove(wave, wave + count, head * sizeof(float));
            note(p, "wave-move-bytes", head * sizeof(float), static_cast<int>(ch));
        } else {
            std::fill(wave, wave + head, 0.f);
            note(p, "wave-zero-bytes", head * sizeof(float), static_cast<int>(ch));
        }
        std::memcpy(wave + head, scratch.data() + static_cast<size_t>(ch) * count, count * sizeof(float));
        note(p, "wave-copy-bytes", count * sizeof(float), static_cast<int>(ch));
    }
    state.valid = true;
    state.filledThrough = written;
    ++state.revision;
    note(p, "wave-commits", 1, -1);
    result.haveWave = result.waveChanged = true;
    return result;
}

void advanceSpectraMono(const ChartRefreshParams& p, ChartBuffers& b, uint64_t written) {
    if (!p.reader || !b.nextEnd || !b.specWin || !b.work || !b.mag) return;
    if (b.specWin->size() < p.fftWin || b.work->size() < p.fftWin) return;
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

void advanceSpectraMulti(const ChartRefreshParams& p, ChartBuffers& b, uint64_t written) {
    if (!p.reader || !b.nextEnd || !b.channelSpecWindows || !b.channelSpecs || !b.work || !b.mag)
        return;
    if (b.work->size() < p.fftWin) return;
    const uint32_t nCh = std::min(
        b.channelCount,
        std::min(static_cast<uint32_t>(b.channelSpecWindows->size()),
                 b.channelSpecCount));
    if (nCh == 0) return;
    for (uint32_t ch = 0; ch < nCh; ++ch) {
        if ((*b.channelSpecWindows)[ch].size() < p.fftWin) return;
    }
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
            if (b.channelSpecs[ch]) {
                FrameBracket history(p.probe, FrameSlice::SpecHistory);
                b.channelSpecs[ch]->pushColumn(*b.mag);
                note(p, "spec-hops", 1, static_cast<int>(ch));
                note(p, "spec-move-bytes", static_cast<uint64_t>(b.channelSpecs[ch]->rows()) *
                    static_cast<uint64_t>(b.channelSpecs[ch]->cols() - 1) * sizeof(float), static_cast<int>(ch));
            }
        }
    });
}

} // namespace

ChartRefreshResult refreshCharts(const ChartRefreshParams& params, ChartBuffers& buffers) {
    FrameBracket refresh(params.probe, FrameSlice::ChartRefresh);
    ChartRefreshResult result;
    if (buffers.waveState) {
        auto& state = *buffers.waveState;
        const uint32_t channels = buffers.channelCount >= 2 ? buffers.channelCount : 1;
        if (state.samples != buffers.waveN || state.sampleRate != buffers.waveSr || state.channels != channels) {
            if (state.samples != 0) state.invalidate();
            state.samples = buffers.waveN;
            state.sampleRate = buffers.waveSr;
            state.channels = channels;
        }
        if (!waveBuffersValid(buffers)) state.valid = false;
    }
    if (params.frozen) {
        result.haveWave = buffers.waveState && buffers.waveState->valid;
        return result;
    }
    if (!params.streamActive || !params.reader) return result;
    const uint64_t written = params.reader->written();
    result = refreshWave(params, buffers, written);
    if (buffers.specSr > 0 && buffers.nextEnd) {
        if (buffers.channelCount >= 2 && buffers.channelWaves)
            advanceSpectraMulti(params, buffers, written);
        else
            advanceSpectraMono(params, buffers, written);
    }

    return result;
}

} // namespace wa
