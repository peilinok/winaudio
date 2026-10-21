#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>
#include "WaveEnvelope.h"

using wa::MinMaxPyramid;
using wa::ensureMinMaxPyramid;
using wa::fillWaveEnvelope;
using wa::fillWaveEnvelopeExact;
using wa::kWaveEnvBlock;
using wa::waveDbWarp;

TEST(WaveEnvelope, DbWarpFloorAndPeak) {
    EXPECT_FLOAT_EQ(waveDbWarp(0.f), 0.f);
    EXPECT_FLOAT_EQ(waveDbWarp(1.f), 1.f);
    EXPECT_FLOAT_EQ(waveDbWarp(-1.f), -1.f);
    EXPECT_FLOAT_EQ(waveDbWarp(1e-8f), 0.f);
}

TEST(WaveEnvelope, ExactMatchesDirectMinMax) {
    // Three columns over 9 samples: [0..3), [3..6), [6..9).
    const float wave[] = {0.1f, -0.5f, 0.2f, 0.8f, -0.1f, 0.0f, -0.9f, 0.3f, 0.4f};
    float x[3], mn[3], mx[3];
    ASSERT_TRUE(fillWaveEnvelopeExact(wave, 9, 0, 8, 3, 1.0, x, mn, mx));
    EXPECT_FLOAT_EQ(mn[0], waveDbWarp(-0.5f));
    EXPECT_FLOAT_EQ(mx[0], waveDbWarp(0.2f));
    EXPECT_FLOAT_EQ(mn[1], waveDbWarp(-0.1f));
    EXPECT_FLOAT_EQ(mx[1], waveDbWarp(0.8f));
    EXPECT_FLOAT_EQ(mn[2], waveDbWarp(-0.9f));
    EXPECT_FLOAT_EQ(mx[2], waveDbWarp(0.4f));
}

TEST(WaveEnvelope, PyramidReusedForSameEpoch) {
    constexpr int kN = kWaveEnvBlock * 8;
    std::vector<float> wave(static_cast<size_t>(kN), 0.f);
    for (int i = 0; i < kN; ++i)
        wave[static_cast<size_t>(i)] = std::sin(0.01f * static_cast<float>(i));

    MinMaxPyramid pyr;
    ensureMinMaxPyramid(pyr, wave.data(), kN, /*epoch=*/7);
    EXPECT_EQ(pyr.epoch, 7u);
    EXPECT_EQ(pyr.n, kN);
    const float* minPtr = pyr.blockMin.data();
    ensureMinMaxPyramid(pyr, wave.data(), kN, /*epoch=*/7);
    EXPECT_EQ(pyr.blockMin.data(), minPtr);  // no realloc/rebuild

    wave[0] = 0.99f;  // mutate source; same epoch must not rebuild
    ensureMinMaxPyramid(pyr, wave.data(), kN, /*epoch=*/7);
    EXPECT_EQ(pyr.blockMin.data(), minPtr);
    EXPECT_FLOAT_EQ(pyr.blockMin[0], std::sin(0.f));  // stale vs mutated â€?by design

    ensureMinMaxPyramid(pyr, wave.data(), kN, /*epoch=*/8);
    EXPECT_EQ(pyr.epoch, 8u);
    EXPECT_FLOAT_EQ(pyr.blockMax[0], wave[0]);  // rebuilt â€?peak sits in block 0
}

TEST(WaveEnvelope, PyramidPathCloseToExactWhenSppLarge) {
    constexpr int kN = 491520;  // kSpecCols * kFftHop
    constexpr int kCols = 960;
    std::vector<float> wave(static_cast<size_t>(kN));
    for (int i = 0; i < kN; ++i) {
        const float t = static_cast<float>(i) * 0.001f;
        wave[static_cast<size_t>(i)] = 0.7f * std::sin(t) + 0.2f * std::sin(t * 7.f);
    }

    std::vector<float> xA(kCols), mnA(kCols), mxA(kCols);
    std::vector<float> xB(kCols), mnB(kCols), mxB(kCols);
    ASSERT_TRUE(fillWaveEnvelopeExact(wave.data(), kN, 0, kN - 1, kCols, 1.0 / 48000.0,
                                      xA.data(), mnA.data(), mxA.data()));

    MinMaxPyramid pyr;
    ASSERT_TRUE(fillWaveEnvelope(wave.data(), kN, 0, kN - 1, kCols, 1.0 / 48000.0, &pyr,
                                 /*epoch=*/1, xB.data(), mnB.data(), mxB.data()));

    // Block snap can widen peaks slightly; stay within a small absolute tolerance on the
    // warped [-1,1] scale (sub-pixel for spp ~ 512).
    double maxDiff = 0.0;
    for (int c = 0; c < kCols; ++c) {
        maxDiff = std::max(maxDiff, std::abs(static_cast<double>(mnA[static_cast<size_t>(c)] -
                                                                 mnB[static_cast<size_t>(c)])));
        maxDiff = std::max(maxDiff, std::abs(static_cast<double>(mxA[static_cast<size_t>(c)] -
                                                                 mxB[static_cast<size_t>(c)])));
    }
    EXPECT_LT(maxDiff, 0.05);
}

// Device-free timing: 8 channels @ full history (reference-scene envelope shape).
TEST(WaveEnvelope, EightChannelFullHistoryTiming) {
    constexpr int kN = 491520;
    constexpr int kCols = 960;
    constexpr int kCh = 8;
    std::vector<float> wave(static_cast<size_t>(kN), 0.25f);
    for (int i = 0; i < kN; i += 97)
        wave[static_cast<size_t>(i)] = (i & 1) ? -0.9f : 0.9f;

    std::vector<float> x(kCols), mn(kCols), mx(kCols);
    std::vector<MinMaxPyramid> pyrs(kCh);

    // Warmup exact
    for (int w = 0; w < 2; ++w)
        for (int ch = 0; ch < kCh; ++ch)
            fillWaveEnvelopeExact(wave.data(), kN, 0, kN - 1, kCols, 1.0 / 48000.0,
                                  x.data(), mn.data(), mx.data());

    constexpr int kIters = 16;
    const auto tExact0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        for (int ch = 0; ch < kCh; ++ch)
            fillWaveEnvelopeExact(wave.data(), kN, 0, kN - 1, kCols, 1.0 / 48000.0,
                                  x.data(), mn.data(), mx.data());
    }
    const auto tExact1 = std::chrono::steady_clock::now();
    const double exactNs =
        std::chrono::duration<double, std::nano>(tExact1 - tExact0).count() / kIters;

    // Warmup pyramid (includes first build)
    for (int ch = 0; ch < kCh; ++ch)
        fillWaveEnvelope(wave.data(), kN, 0, kN - 1, kCols, 1.0 / 48000.0, &pyrs[static_cast<size_t>(ch)],
                         /*epoch=*/1, x.data(), mn.data(), mx.data());

    const auto tPyr0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        for (int ch = 0; ch < kCh; ++ch)
            fillWaveEnvelope(wave.data(), kN, 0, kN - 1, kCols, 1.0 / 48000.0,
                             &pyrs[static_cast<size_t>(ch)], /*epoch=*/1, x.data(), mn.data(),
                             mx.data());
    }
    const auto tPyr1 = std::chrono::steady_clock::now();
    const double pyrNs =
        std::chrono::duration<double, std::nano>(tPyr1 - tPyr0).count() / kIters;

    std::cout << "slice=waveform-envelope-exact N=" << kCh << " samples=" << kN
              << " cols=" << kCols << " nanos_per_frame=" << static_cast<long long>(exactNs)
              << "\n";
    std::cout << "slice=waveform-envelope-pyramid-cached N=" << kCh << " samples=" << kN
              << " cols=" << kCols << " nanos_per_frame=" << static_cast<long long>(pyrNs)
              << "\n";

    // Cached pyramid query must beat the admitted ~0.49 ms exact scan by a wide margin.
    EXPECT_LT(pyrNs, exactNs * 0.25);
#if defined(NDEBUG)
    EXPECT_LT(pyrNs, 250'000.0);  // Release: << admitted ~0.49 ms exact; machine noise OK
#else
    // Debug CI runners are ~4x slower; keep relative bound only for absolute floor.
    EXPECT_LT(pyrNs, 2'000'000.0);
#endif
}