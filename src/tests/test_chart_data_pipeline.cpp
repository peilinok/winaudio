#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "ChartDataPipeline.h"
#include "FakeScopeReader.h"
#include "Spectrogram.h"

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

    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 128);
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

    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 64);
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

    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 256);
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

    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 64);
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

    ChartBuffers b;
    b.channelWaves = &chWaves;
    b.channelCount = 2;
    b.waveN = 4;
    b.waveSr = 48000;
    b.channelSpecWindows = &chSpecWin;
    b.channelSpecs = &chSpecs;
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

    ChartBuffers b = makeMonoBuffers(wave, specWin, work, mag, spec, nextEnd, 8);
    ASSERT_TRUE(refreshCharts(p, b).haveWave);
    // head zeros, tail = samples
    EXPECT_FLOAT_EQ(wave[0], 0.f);
    EXPECT_FLOAT_EQ(wave[4], 0.f);
    EXPECT_FLOAT_EQ(wave[5], 10.f);
    EXPECT_FLOAT_EQ(wave[6], 20.f);
    EXPECT_FLOAT_EQ(wave[7], 30.f);
}
