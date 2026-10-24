#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
#include "ChartDataPipeline.h"
#include "FakeScopeReader.h"
#include "Spectrogram.h"
#include "FrameProbe.h"
#include "ScopeBuffer.h"
#include "WaveEnvelope.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

using namespace wa;

namespace {

constexpr size_t kWin = 64;
constexpr size_t kHop = 32;
constexpr size_t kCatch = 8;

// Fill fake reader with a long mono ramp so hop windows are distinct.
void fillRamp(FakeScopeReader& r, size_t frames) {
    std::vector<float> s(frames);
    for (size_t i = 0; i < frames; ++i) s[i] = static_cast<float>(i);
    r.pushMono(s.data(), frames);
}

ChartBuffers makeMonoBuffers(std::vector<float>& wave, std::vector<float>& specWin,
                             std::vector<std::complex<float>>& work, std::vector<float>& mag,
                             Spectrogram& spec, uint64_t& nextEnd, int waveN) {
    ChartBuffers b;
    b.wave = wave.data();
    b.waveN = waveN;
    b.waveSr = 48000;
    b.specWin = &specWin;
    b.work = &work;
    b.mag = &mag;
    b.spectrogram = &spec;
    b.nextEnd = &nextEnd;
    b.specSr = 48000;
    return b;
}

} // namespace

namespace {
class InterruptibleScopeReader final : public ScopeReader {
public:
    FakeScopeReader data{3};
    int failChannel = -1;
    mutable unsigned reads = 0;
    uint64_t written() const override { return data.written(); }
    uint16_t channels() const override { return data.channels(); }
    bool snapshotLatest(size_t n, float* out, uint64_t& end) const override {
        return data.snapshotLatest(n, out, end);
    }
    bool snapshotEndingAt(uint64_t end, size_t n, float* out) const override {
        ++reads;
        if (failChannel == 0) { std::fill(out, out + n, -999.f); return false; }
        return data.snapshotEndingAt(end, n, out);
    }
    bool snapshotChannelEndingAt(uint16_t ch, uint64_t end, size_t n, float* out) const override {
        ++reads;
        if (failChannel == static_cast<int>(ch)) {
            std::fill(out, out + n, -999.f);
            return false;
        }
        return data.snapshotChannelEndingAt(ch, end, n, out);
    }
};

class WrappingScopeReader final : public ScopeReader {
public:
    mutable ScopeBuffer data{8, 3};
    mutable bool overwrite = false;
    mutable unsigned writtenReads = 0;
    uint64_t written() const override { ++writtenReads; return data.totalWritten(); }
    uint16_t channels() const override { return data.channels(); }
    bool snapshotLatest(size_t n, float* out, uint64_t& end) const override {
        return data.snapshotLatest(n, out, end);
    }
    bool snapshotEndingAt(uint64_t end, size_t n, float* out) const override {
        return data.snapshotEndingAt(end, n, out);
    }
    bool snapshotChannelEndingAt(uint16_t ch, uint64_t end, size_t n, float* out) const override {
        if (overwrite && ch == 1) {
            overwrite = false;
            const float zeros[8]{};
            data.push(zeros, 8);
        }
        return data.snapshotChannelEndingAt(ch, end, n, out);
    }
};
}

TEST(ChartDataPipeline, FailedChannelPreservesTheEntireCommittedWindow) {
    InterruptibleScopeReader reader;
    const float first[] = {1, 10, 100, 2, 20, 200, 3, 30, 300, 4, 40, 400};
    reader.data.pushInterleaved(first, 4);
    std::vector<std::vector<float>> waves(3, std::vector<float>(4));
    WaveRefreshState filled;
    WaveSnapshotScratch buffersScratch;
    ChartBuffers buffers;
    buffers.waveScratch = &buffersScratch;
    buffers.channelWaves = &waves;
    buffers.channelCount = 3;
    buffers.waveN = 4;
    buffers.waveSr = 48000;
    buffers.waveState = &filled;
    ChartRefreshParams params;
    params.reader = &reader;
    params.streamActive = true;
    ASSERT_TRUE(refreshCharts(params, buffers).haveWave);
    const auto committed = waves;
    const float tail[] = {5, 50, 500};
    reader.data.pushInterleaved(tail, 1);
    reader.failChannel = 1;
    EXPECT_TRUE(refreshCharts(params, buffers).haveWave);
    EXPECT_EQ(waves, committed);
    EXPECT_EQ(filled.filledThrough, 4u);
}

TEST(ChartDataPipeline, EveryChannelFailurePreservesInitialIncrementalAndFullWindows) {
    for (int failed = 0; failed < 3; ++failed) {
        for (size_t advance : {0u, 1u, 4u, 5u}) {
            SCOPED_TRACE(testing::Message() << "channel=" << failed << " advance=" << advance);
            InterruptibleScopeReader reader;
            const float first[] = {1, 2, 3, 4};
            reader.data.pushMono(first, 4);
            std::vector<std::vector<float>> waves(3, std::vector<float>(4, -7.f));
            WaveRefreshState state;
            WaveSnapshotScratch scratch;
            ChartBuffers b;
            b.channelWaves = &waves;
            b.channelCount = 3;
            b.waveN = 4;
            b.waveSr = 48000;
            b.waveState = &state;
            b.waveScratch = &scratch;
            ChartRefreshParams p;
            p.reader = &reader;
            p.streamActive = true;
            if (advance) ASSERT_TRUE(refreshCharts(p, b).waveChanged);
            const auto old = waves;
            const uint64_t revision = state.revision, end = state.filledThrough;
            const float tail[] = {5, 6, 7, 8, 9};
            if (advance) reader.data.pushMono(tail, advance);
            reader.failChannel = failed;
            const auto result = refreshCharts(p, b);
            EXPECT_EQ(result.haveWave, advance != 0);
            EXPECT_FALSE(result.waveChanged);
            EXPECT_EQ(waves, old);
            EXPECT_EQ(state.revision, revision);
            EXPECT_EQ(state.filledThrough, end);
            reader.failChannel = -1;
            ASSERT_TRUE(refreshCharts(p, b).waveChanged);
            EXPECT_EQ(state.revision, revision + 1);
            std::vector<float> reference(4);
            for (uint16_t ch = 0; ch < 3; ++ch) {
                ASSERT_TRUE(reader.data.snapshotChannelEndingAt(ch, reader.data.written(), 4, reference.data()));
                EXPECT_EQ(waves[ch], reference);
            }
        }
    }
}

TEST(ChartDataPipeline, FrozenUnchangedAndSilentWindowsHaveDistinctAvailabilityAndChange) {
    InterruptibleScopeReader reader;
    const float silence[] = {0, 0};
    reader.data.pushMono(silence, 2);
    std::vector<float> wave(4, -1.f);
    WaveRefreshState state;
    WaveSnapshotScratch scratch;
    ChartBuffers b;
    b.wave = wave.data(); b.waveN = 4; b.waveSr = 48000;
    b.waveState = &state; b.waveScratch = &scratch;
    ChartRefreshParams p;
    p.reader = &reader; p.streamActive = true; p.frozen = true;
    EXPECT_FALSE(refreshCharts(p, b).haveWave);
    EXPECT_EQ(reader.reads, 0u);
    p.frozen = false;
    ASSERT_TRUE(refreshCharts(p, b).waveChanged);
    EXPECT_EQ(wave, (std::vector<float>{0, 0, 0, 0}));
    const auto reads = reader.reads;
    const auto revision = state.revision;
    EXPECT_TRUE(refreshCharts(p, b).haveWave);
    EXPECT_FALSE(refreshCharts(p, b).waveChanged);
    EXPECT_EQ(reader.reads, reads);
    EXPECT_EQ(state.revision, revision);
    p.frozen = true;
    reader.data.pushMono(silence, 2);
    EXPECT_FALSE(refreshCharts(p, b).waveChanged);
    EXPECT_EQ(reader.reads, reads);
    p.frozen = false;
    ASSERT_TRUE(refreshCharts(p, b).waveChanged);
    EXPECT_EQ(state.revision, revision + 1);
    p.streamActive = false;
    EXPECT_FALSE(refreshCharts(p, b).haveWave);
}

TEST(ChartDataPipeline, NewSourceAndRollbackDoNotReuseOldValidityOrCommitRevision) {
    InterruptibleScopeReader reader;
    const float samples[] = {1, 2, 3, 4};
    reader.data.pushMono(samples, 4);
    std::vector<float> wave(4);
    WaveRefreshState state;
    WaveSnapshotScratch scratch;
    ChartBuffers b;
    b.wave = wave.data(); b.waveN = 4; b.waveSr = 48000;
    b.waveState = &state; b.waveScratch = &scratch;
    ChartRefreshParams p;
    p.reader = &reader; p.streamActive = true;
    ASSERT_TRUE(refreshCharts(p, b).waveChanged);
    const auto generation = state.generation;
    state.invalidate();
    reader.failChannel = 0;
    EXPECT_FALSE(refreshCharts(p, b).haveWave);
    EXPECT_GT(state.generation, generation);
    EXPECT_EQ(state.revision, 0u);
    reader.failChannel = -1;
    ASSERT_TRUE(refreshCharts(p, b).waveChanged);
    const auto revision = state.revision;
    reader.data.setWritten(2);
    reader.failChannel = 0;
    EXPECT_FALSE(refreshCharts(p, b).haveWave);
    EXPECT_EQ(state.revision, revision);
    reader.failChannel = -1;
    ASSERT_TRUE(refreshCharts(p, b).waveChanged);
    EXPECT_GT(state.revision, revision);
    EXPECT_EQ(wave, (std::vector<float>{0, 0, 1, 2}));
    p.frozen = true;
    b.waveSr = 44100;
    EXPECT_FALSE(refreshCharts(p, b).haveWave);
}

TEST(ChartDataPipeline, EnvelopeIdentityIncludesSourceGenerationAndIndependentDirections) {
    std::vector<float> capture(256, .1f), render(256, .9f);
    MinMaxPyramid cap, ren;
    WaveEnvelopeWork work;
    ensureMinMaxPyramid(cap, capture.data(), 256, 1, &work, 3);
    ensureMinMaxPyramid(ren, render.data(), 256, 1, &work, 3);
    EXPECT_FLOAT_EQ(cap.blockMax[0], .1f);
    EXPECT_FLOAT_EQ(ren.blockMax[0], .9f);
    work = {};
    ensureMinMaxPyramid(cap, capture.data(), 256, 1, &work, 3);
    EXPECT_EQ(work.rebuildSamples, 0u);
    ensureMinMaxPyramid(cap, render.data(), 256, 1, &work, 4);
    EXPECT_EQ(work.rebuildSamples, 256u);
    EXPECT_FLOAT_EQ(cap.blockMax[0], .9f);
}

TEST(ChartDataPipeline, PhysicalWrapAndOverwrittenWindowKeepTheLastCompleteGroup) {
    WrappingScopeReader reader;
    const float first[] = {1, 2, 3, 4};
    reader.data.push(first, 4);
    std::vector<std::vector<float>> waves(3, std::vector<float>(4));
    WaveRefreshState state;
    WaveSnapshotScratch scratch;
    ChartBuffers b;
    b.channelWaves = &waves; b.channelCount = 3; b.waveN = 4; b.waveSr = 48000;
    b.waveState = &state; b.waveScratch = &scratch;
    ChartRefreshParams p;
    p.reader = &reader; p.streamActive = true;
    ASSERT_TRUE(refreshCharts(p, b).waveChanged);
    const float next[] = {5, 6, 7, 8, 9, 10};
    reader.data.push(next, 6);
    ASSERT_TRUE(refreshCharts(p, b).waveChanged);
    for (const auto& wave : waves) EXPECT_EQ(wave, (std::vector<float>{7, 8, 9, 10}));
    const auto old = waves;
    const auto revision = state.revision;
    const auto generation = state.generation;
    const float tail[] = {11};
    reader.data.push(tail, 1);
    reader.overwrite = true;
    reader.writtenReads = 0;
    const auto failed = refreshCharts(p, b);
    EXPECT_TRUE(failed.haveWave);
    EXPECT_FALSE(failed.waveChanged);
    EXPECT_EQ(reader.writtenReads, 1u);
    EXPECT_EQ(waves, old);
    EXPECT_EQ(state.revision, revision);
    const auto* allocation = scratch.samples.data();
    ASSERT_TRUE(refreshCharts(p, b).waveChanged);
    for (const auto& wave : waves) EXPECT_EQ(wave, (std::vector<float>{0, 0, 0, 0}));
    EXPECT_EQ(state.generation, generation);
    EXPECT_EQ(scratch.samples.data(), allocation);
    EXPECT_EQ(scratch.samples.capacity(), 12u);
}

TEST(ChartDataPipeline, WorkRecordsDistinguishSnapshotMoveAndUnchangedReuse) {
    FakeScopeReader reader(1);
    fillRamp(reader, 8);
    std::vector<float> wave(8);
    WaveRefreshState filled;
    WaveSnapshotScratch buffersScratch;
    ChartBuffers buffers;
    buffers.waveScratch = &buffersScratch;
    buffers.wave = wave.data();
    buffers.waveN = 8;
    buffers.waveSr = 48000;
    buffers.waveState = &filled;
    FrameProbe probe;
    probe.setEnabled(true);
    ChartRefreshParams params;
    params.reader = &reader;
    params.streamActive = true;
    params.probe = &probe;
    params.source = 17;
    params.direction = 1;
    const auto total = [&](const char* metric) {
        uint64_t value = 0;
        for (size_t i = 0; i < probe.workCount(); ++i) {
            char line[512];
            EXPECT_GT(probe.formatWork(i, line, sizeof(line)), 0);
            const std::string item(line);
            EXPECT_NE(item.find("source=17 direction=1"), std::string::npos);
            if (item.find(std::string("metric=") + metric + " ") != std::string::npos) {
                const size_t begin = item.find("value=");
                value += std::stoull(item.substr(begin + 6));
            }
        }
        return value;
    };
    probe.beginFrame();
    ASSERT_TRUE(refreshCharts(params, buffers).haveWave);
    EXPECT_EQ(total("wave-request-samples"), 8u);
    EXPECT_EQ(total("wave-move-bytes"), 0u);
    EXPECT_EQ(total("wave-copy-bytes"), 32u);
    probe.endFrame();
    const float tail[] = {8, 9};
    reader.pushMono(tail, 2);
    probe.beginFrame();
    ASSERT_TRUE(refreshCharts(params, buffers).haveWave);
    EXPECT_EQ(total("wave-request-samples"), 2u);
    EXPECT_EQ(total("wave-move-bytes"), 24u);
    EXPECT_EQ(total("wave-copy-bytes"), 8u);
    EXPECT_FLOAT_EQ(wave.back(), 9.f);
    probe.endFrame();
    probe.beginFrame();
    ASSERT_TRUE(refreshCharts(params, buffers).haveWave);
    EXPECT_EQ(total("wave-request-samples"), 0u);
    EXPECT_EQ(total("wave-move-bytes"), 0u);
    EXPECT_EQ(total("wave-copy-bytes"), 0u);
    probe.endFrame();
}

TEST(ChartDataPipeline, FrozenDoesNotAdvanceCursorOrWave) {
    FakeScopeReader r(1);
    fillRamp(r, 512);

    std::vector<float> wave(128, 0.f);
    std::vector<float> specWin(kWin, 0.f);
    std::vector<std::complex<float>> work(kWin);
    std::vector<float> mag;
    Spectrogram spec(16, 32, 20.0, 24000.0, 48000);
    uint64_t nextEnd = 0;

    ChartRefreshParams p;
    p.reader = &r;
    p.streamActive = true;
    p.frozen = false;
    p.fftWin = kWin;
    p.fftHop = kHop;
    p.maxCatchup = kCatch;

    WaveRefreshState bState;
    WaveSnapshotScratch bScratch;
    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 128);
    b.waveScratch = &bScratch;
    b.waveState = &bState;
    auto live = refreshCharts(p, b);
    EXPECT_TRUE(live.haveWave);
    const uint64_t endAfterLive = nextEnd;
    EXPECT_GT(endAfterLive, 0u);
    const float wave0 = wave[100];

    p.frozen = true;
    auto frozen = refreshCharts(p, b);
    EXPECT_TRUE(frozen.haveWave);
    EXPECT_EQ(nextEnd, endAfterLive);
    EXPECT_FLOAT_EQ(wave[100], wave0);
}

TEST(ChartDataPipeline, InactiveStreamDoesNotWrite) {
    FakeScopeReader r(1);
    fillRamp(r, 256);
    std::vector<float> wave(64, -1.f);
    std::vector<float> specWin(kWin);
    std::vector<std::complex<float>> work(kWin);
    std::vector<float> mag;
    Spectrogram spec(8, 16, 20.0, 24000.0, 48000);
    uint64_t nextEnd = 0;

    ChartRefreshParams p;
    p.reader = &r;
    p.streamActive = false;
    p.frozen = false;
    p.fftWin = kWin;
    p.fftHop = kHop;

    WaveRefreshState bState;
    WaveSnapshotScratch bScratch;
    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 64);
    b.waveScratch = &bScratch;
    b.waveState = &bState;
    auto res = refreshCharts(p, b);
    EXPECT_FALSE(res.haveWave);
    EXPECT_EQ(nextEnd, 0u);
    EXPECT_FLOAT_EQ(wave[0], -1.f);
}

TEST(ChartDataPipeline, MonoSpectrogramHopsUseDistinctEndingAtWindows) {
    // With hop alignment, each catch-up hop pulls a different ending-at window.
    // Inject enough samples so multiple hops fire in one refresh.
    FakeScopeReader r(1);
    fillRamp(r, kWin + 4 * kHop); // several hop boundaries

    std::vector<float> wave(256, 0.f);
    std::vector<float> specWin(kWin, 0.f);
    std::vector<std::complex<float>> work(kWin);
    std::vector<float> mag;
    Spectrogram spec(8, 16, 20.0, 24000.0, 48000);
    uint64_t nextEnd = 0;

    ChartRefreshParams p;
    p.reader = &r;
    p.streamActive = true;
    p.frozen = false;
    p.fftWin = kWin;
    p.fftHop = kHop;
    p.maxCatchup = kCatch;

    WaveRefreshState bState;
    WaveSnapshotScratch bScratch;
    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 256);
    b.waveScratch = &bScratch;
    b.waveState = &bState;
    refreshCharts(p, b);

    // First emission at window size, then +hop each time.
    EXPECT_GE(nextEnd, static_cast<uint64_t>(kWin));
    EXPECT_EQ((nextEnd - static_cast<uint64_t>(kWin)) % static_cast<uint64_t>(kHop), 0u);

    // Last hop window content: samples [nextEnd-kWin, nextEnd)
    float expectedLast[kWin];
    ASSERT_TRUE(r.snapshotEndingAt(nextEnd, kWin, expectedLast));
    // specWin holds the last successful hop's samples after refresh.
    for (size_t i = 0; i < kWin; ++i)
        EXPECT_FLOAT_EQ(specWin[i], expectedLast[i]) << "i=" << i;
}

TEST(ChartDataPipeline, CatchupCapsHopsPerRefresh) {
    FakeScopeReader r(1);
    // Far ahead of analysis: many pending hops.
    fillRamp(r, kWin + 100 * kHop);

    std::vector<float> wave(64, 0.f);
    std::vector<float> specWin(kWin);
    std::vector<std::complex<float>> work(kWin);
    std::vector<float> mag;
    Spectrogram spec(8, 16, 20.0, 24000.0, 48000);
    uint64_t nextEnd = 0;

    ChartRefreshParams p;
    p.reader = &r;
    p.streamActive = true;
    p.fftWin = kWin;
    p.fftHop = kHop;
    p.maxCatchup = 3; // tight cap

    WaveRefreshState bState;
    WaveSnapshotScratch bScratch;
    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 64);
    b.waveScratch = &bScratch;
    b.waveState = &bState;
    refreshCharts(p, b);

    // After fast-forward + at most maxCatchup emissions, cursor stays near the end.
    // Max emissions in one call is maxCatchup; first seed then process.
    // nextEnd should not equal written if backlog was huge... actually advanceAnalysis
    // fast-forwards then processes up to remaining which is maxCatchup hops.
    // So nextEnd = written - (written-next) % hop after processing maxCatchup from near end.
    EXPECT_GT(nextEnd, 0u);
    EXPECT_LE(nextEnd, r.written());
    // Distance from written to nextEnd should be < hop (fully caught to last boundary).
    EXPECT_LT(r.written() - nextEnd, static_cast<uint64_t>(kHop));
}

TEST(ChartDataPipeline, MultiChannelWavesShareWrittenEnd) {
    FakeScopeReader r(2);
    // 4 frames stereo
    const float in[] = {
        1, 10, 2, 20, 3, 30, 4, 40, 5, 50, 6, 60, 7, 70, 8, 80,
    };
    r.pushInterleaved(in, 8);

    std::vector<std::vector<float>> chWaves(2, std::vector<float>(4, 0.f));
    std::vector<std::vector<float>> chSpecWin(2, std::vector<float>(kWin, 0.f));
    std::vector<std::unique_ptr<Spectrogram>> chSpecs;
    chSpecs.push_back(std::make_unique<Spectrogram>(8, 8, 20.0, 24000.0, 48000));
    chSpecs.push_back(std::make_unique<Spectrogram>(8, 8, 20.0, 24000.0, 48000));
    std::vector<std::complex<float>> work(kWin);
    std::vector<float> mag;
    uint64_t nextEnd = 0;

    ChartRefreshParams p;
    p.reader = &r;
    p.streamActive = true;
    p.fftWin = kWin;
    p.fftHop = kHop;

    WaveRefreshState bState;
    WaveSnapshotScratch bScratch;
    ChartBuffers b;
    b.waveScratch = &bScratch;
    b.waveState = &bState;
    b.channelWaves = &chWaves;
    b.channelCount = 2;
    b.waveN = 4;
    b.waveSr = 48000;
    b.channelSpecWindows = &chSpecWin;
    std::vector<Spectrogram*> chSpecsPointers;
    for (auto& history : chSpecs) chSpecsPointers.push_back(history.get());
    b.channelSpecs = chSpecsPointers.data();
    b.channelSpecCount = static_cast<uint32_t>(chSpecsPointers.size());
    b.work = &work;
    b.mag = &mag;
    b.nextEnd = &nextEnd;
    b.specSr = 48000;

    auto res = refreshCharts(p, b);
    EXPECT_TRUE(res.haveWave);
    // Latest 4 frames: 5..8 on ch0, 50..80 on ch1
    EXPECT_FLOAT_EQ(chWaves[0][0], 5.f);
    EXPECT_FLOAT_EQ(chWaves[0][3], 8.f);
    EXPECT_FLOAT_EQ(chWaves[1][0], 50.f);
    EXPECT_FLOAT_EQ(chWaves[1][3], 80.f);
}

TEST(ChartDataPipeline, WaveProgressiveFillPutsNewestAtTail) {
    FakeScopeReader r(1);
    const float s[] = {10.f, 20.f, 30.f};
    r.pushMono(s, 3);

    std::vector<float> wave(8, -9.f);
    std::vector<float> specWin(kWin);
    std::vector<std::complex<float>> work(kWin);
    std::vector<float> mag;
    Spectrogram spec(4, 4, 20.0, 24000.0, 48000);
    uint64_t nextEnd = 0;

    ChartRefreshParams p;
    p.reader = &r;
    p.streamActive = true;
    p.fftWin = kWin;
    p.fftHop = kHop;

    WaveRefreshState bState;
    WaveSnapshotScratch bScratch;
    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 8);
    b.waveScratch = &bScratch;
    b.waveState = &bState;
    ASSERT_TRUE(refreshCharts(p, b).haveWave);
    // head zeros, tail = samples
    EXPECT_FLOAT_EQ(wave[0], 0.f);
    EXPECT_FLOAT_EQ(wave[4], 0.f);
    EXPECT_FLOAT_EQ(wave[5], 10.f);
    EXPECT_FLOAT_EQ(wave[6], 20.f);
    EXPECT_FLOAT_EQ(wave[7], 30.f);
}

// Device-free record for live 8-channel refreshCharts at the GUI sizes:
// each call advances written by maxCatchup hops (incremental wave + catch-up FFTs).
// No time limit: the line is the citeable record, not a pass bar.
TEST(ChartDataPipeline, RefreshCharts8chRecord) {
    constexpr uint16_t kChannels = 8;
    constexpr size_t kFftWin = 2048;
    constexpr size_t kFftHop = 1024;
    constexpr size_t kCatchup = 8;
    constexpr int kSpecRows = 128;
    constexpr int kSpecCols = 480;
    constexpr int kWaveN = kSpecCols * static_cast<int>(kFftHop);
    constexpr int kCalls = 16;
    constexpr uint32_t kSr = 48000;
    constexpr size_t kAdvance = kCatchup * kFftHop;

    FakeScopeReader reader(kChannels);
    std::vector<float> chunk(4096 * kChannels);
    auto pushFrames = [&](size_t frames, uint32_t& filled) {
        size_t left = frames;
        while (left > 0) {
            const size_t nfr = std::min(left, static_cast<size_t>(4096));
            for (size_t i = 0; i < nfr; ++i) {
                const float s = static_cast<float>((filled + i) & 1023) / 1023.f;
                for (uint16_t ch = 0; ch < kChannels; ++ch)
                    chunk[i * kChannels + ch] = s;
            }
            reader.pushInterleaved(chunk.data(), nfr);
            filled += static_cast<uint32_t>(nfr);
            left -= nfr;
        }
    };

    uint32_t filled = 0;
    pushFrames(static_cast<size_t>(kWaveN), filled);

    std::vector<std::vector<float>> channelWaves(
        kChannels, std::vector<float>(static_cast<size_t>(kWaveN), 0.f));
    std::vector<std::vector<float>> channelSpecWindows(
        kChannels, std::vector<float>(kFftWin, 0.f));
    std::vector<std::unique_ptr<Spectrogram>> channelSpecs;
    channelSpecs.reserve(kChannels);
    for (uint16_t ch = 0; ch < kChannels; ++ch) {
        channelSpecs.push_back(std::make_unique<Spectrogram>(
            kSpecRows, kSpecCols, 20.0, static_cast<double>(kSr) / 2.0, kSr));
    }
    std::vector<std::complex<float>> work(kFftWin);
    std::vector<float> mag(kFftWin / 2 + 1, 0.f);
    uint64_t nextEnd = 0;
    WaveRefreshState waveFilled;

    ChartRefreshParams params;
    params.reader = &reader;
    params.streamActive = true;
    params.fftWin = kFftWin;
    params.fftHop = kFftHop;
    params.maxCatchup = kCatchup;

    WaveSnapshotScratch buffersScratch;
    ChartBuffers buffers;
    buffers.waveScratch = &buffersScratch;
    buffers.channelWaves = &channelWaves;
    buffers.channelCount = kChannels;
    buffers.waveN = kWaveN;
    buffers.waveSr = kSr;
    buffers.waveState = &waveFilled;
    buffers.channelSpecWindows = &channelSpecWindows;
    std::vector<Spectrogram*> channelSpecsPointers;
    for (auto& history : channelSpecs) channelSpecsPointers.push_back(history.get());
    buffers.channelSpecs = channelSpecsPointers.data();
    buffers.channelSpecCount = static_cast<uint32_t>(channelSpecsPointers.size());
    buffers.work = &work;
    buffers.mag = &mag;
    buffers.nextEnd = &nextEnd;
    buffers.specSr = kSr;

    ASSERT_TRUE(refreshCharts(params, buffers).haveWave);
    EXPECT_EQ(nextEnd, reader.written());
    EXPECT_EQ(waveFilled.filledThrough, reader.written());

    LARGE_INTEGER freq;
    ASSERT_TRUE(QueryPerformanceFrequency(&freq));
    ASSERT_GT(freq.QuadPart, 0);

    // Live path: each call pushes maxCatchup hops of new audio so both
    // incremental wave fill and FFT catch-up stay on the hot path.
    LARGE_INTEGER t0;
    LARGE_INTEGER t1;
    ASSERT_TRUE(QueryPerformanceCounter(&t0));
    for (int i = 0; i < kCalls; ++i) {
        pushFrames(kAdvance, filled);
        ASSERT_TRUE(refreshCharts(params, buffers).haveWave);
    }
    ASSERT_TRUE(QueryPerformanceCounter(&t1));
    EXPECT_EQ(nextEnd, reader.written());
    EXPECT_EQ(waveFilled.filledThrough, reader.written());

    const uint64_t ticks = static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
    const uint64_t qpcFreq = static_cast<uint64_t>(freq.QuadPart);
    ASSERT_GT(ticks, 1u);
    const uint64_t whole = ticks / qpcFreq;
    const uint64_t rem = ticks % qpcFreq;
    const uint64_t nanos = whole * 1000000000ull + (rem * 1000000000ull) / qpcFreq;

#ifdef NDEBUG
    const char* build = "Release";
#else
    const char* build = "Debug";
#endif
    std::cout << "slice=refreshCharts-8ch"
              << " build=" << build
              << " N=" << kCalls
              << " ticks=" << ticks
              << " qpc_freq=" << qpcFreq
              << " nanos=" << nanos
              << " uncertainty_ticks=1"
              << " hops_per_call=" << kCatchup
              << " wave=incremental"
              << " command=.\\build\\bin\\" << build
              << "\\WinAudioTests.exe --gtest_filter=ChartDataPipeline.RefreshCharts8chRecord"
              << "\n";
}

TEST(ChartDataPipeline, WaveIncrementalMatchesFullRefill) {
    FakeScopeReader r(2);
    const float block1[] = {
        1, 10, 2, 20, 3, 30, 4, 40, 5, 50, 6, 60, 7, 70, 8, 80,
    };
    r.pushInterleaved(block1, 8);

    std::vector<std::vector<float>> wavesA(2, std::vector<float>(8, 0.f));
    std::vector<std::vector<float>> wavesB(2, std::vector<float>(8, 0.f));
    std::vector<std::vector<float>> specWin(2, std::vector<float>(kWin, 0.f));
    std::vector<std::unique_ptr<Spectrogram>> specs;
    specs.push_back(std::make_unique<Spectrogram>(4, 4, 20.0, 24000.0, 48000));
    specs.push_back(std::make_unique<Spectrogram>(4, 4, 20.0, 24000.0, 48000));
    std::vector<std::complex<float>> work(kWin);
    std::vector<float> mag;
    uint64_t nextEnd = 0;
    WaveRefreshState filledA;

    ChartRefreshParams p;
    p.reader = &r;
    p.streamActive = true;
    p.fftWin = kWin;
    p.fftHop = kHop;
    p.maxCatchup = kCatch;

    WaveSnapshotScratch aScratch;
    ChartBuffers a;
    a.waveScratch = &aScratch;
    a.channelWaves = &wavesA;
    a.channelCount = 2;
    a.waveN = 8;
    a.waveSr = 48000;
    a.waveState = &filledA;
    a.channelSpecWindows = &specWin;
    std::vector<Spectrogram*> specsPointers;
    for (auto& history : specs) specsPointers.push_back(history.get());
    a.channelSpecs = specsPointers.data();
    a.channelSpecCount = static_cast<uint32_t>(specsPointers.size());
    a.work = &work;
    a.mag = &mag;
    a.nextEnd = &nextEnd;
    a.specSr = 48000;

    ASSERT_TRUE(refreshCharts(p, a).haveWave);
    EXPECT_EQ(filledA.filledThrough, r.written());

    const float block2[] = {9, 90, 10, 100, 11, 110};
    r.pushInterleaved(block2, 3);
    ASSERT_TRUE(refreshCharts(p, a).haveWave); // incremental (+3)

    // Full refill into B without cursor (same reader state).
    WaveRefreshState bState;
    WaveSnapshotScratch bScratch;
    ChartBuffers b;
    b.waveScratch = &bScratch;
    b.waveState = &bState;
    b.channelWaves = &wavesB;
    b.channelCount = 2;
    b.waveN = 8;
    b.waveSr = 48000;
    b.channelSpecWindows = &specWin;
    b.channelSpecs = specsPointers.data();
    b.channelSpecCount = static_cast<uint32_t>(specsPointers.size());
    b.work = &work;
    b.mag = &mag;
    b.nextEnd = &nextEnd;
    b.specSr = 48000;
    ASSERT_TRUE(refreshCharts(p, b).haveWave);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 8; ++i)
            EXPECT_FLOAT_EQ(wavesA[static_cast<size_t>(ch)][static_cast<size_t>(i)],
                            wavesB[static_cast<size_t>(ch)][static_cast<size_t>(i)])
                << "ch=" << ch << " i=" << i;
}

TEST(ChartDataPipeline, WaveSkipWhenWrittenUnchanged) {
    FakeScopeReader r(1);
    fillRamp(r, 64);

    std::vector<float> wave(32, 0.f);
    std::vector<float> specWin(kWin, 0.f);
    std::vector<std::complex<float>> work(kWin);
    std::vector<float> mag;
    Spectrogram spec(4, 4, 20.0, 24000.0, 48000);
    uint64_t nextEnd = 0;
    WaveRefreshState filled;

    ChartRefreshParams p;
    p.reader = &r;
    p.streamActive = true;
    p.fftWin = kWin;
    p.fftHop = kHop;

    WaveSnapshotScratch bScratch;
    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 32);
    b.waveScratch = &bScratch;
    b.waveState = &filled;
    ASSERT_TRUE(refreshCharts(p, b).haveWave);
    const float mid = wave[16];
    wave[16] = 123.f; // poison; skip path must not rewrite
    ASSERT_TRUE(refreshCharts(p, b).haveWave);
    EXPECT_FLOAT_EQ(wave[16], 123.f);
    EXPECT_EQ(filled.filledThrough, r.written());
    (void)mid;
}
