#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <vector>
#include "HeatmapRgba.h"
#include "Spectrogram.h"

using wa::HeatmapColorStop;
using wa::buildHeatmapLut;
using wa::fillHeatmapRgba;
using wa::packRgba8;

// Mirrors AppUi waSpectroColormap stops (Plasma-like, transparent floor).
static const HeatmapColorStop kStops[] = {
    {0.050f, 0.030f, 0.528f, 0.00f},
    {0.294f, 0.012f, 0.631f, 0.55f},
    {0.492f, 0.012f, 0.658f, 0.90f},
    {0.658f, 0.135f, 0.588f, 1.00f},
    {0.798f, 0.280f, 0.470f, 1.00f},
    {0.902f, 0.425f, 0.353f, 1.00f},
    {0.973f, 0.586f, 0.252f, 1.00f},
    {0.940f, 0.975f, 0.131f, 1.00f},
};

TEST(HeatmapRgba, PackRgba8ByteOrder) {
    EXPECT_EQ(packRgba8(1.f, 0.f, 0.f, 0.f), 0x000000FFu);
    EXPECT_EQ(packRgba8(0.f, 1.f, 0.f, 0.f), 0x0000FF00u);
    EXPECT_EQ(packRgba8(0.f, 0.f, 1.f, 0.f), 0x00FF0000u);
    EXPECT_EQ(packRgba8(0.f, 0.f, 0.f, 1.f), 0xFF000000u);
}

TEST(HeatmapRgba, FloorMapsToTransparentLut0) {
    uint32_t lut[256];
    buildHeatmapLut(kStops, 8, lut);
    EXPECT_EQ((lut[0] >> 24) & 0xFFu, 0u);  // alpha 0 at floor

    const float values[4] = {-96.f, -96.f, -96.f, -96.f};
    uint32_t out[4] = {};
    fillHeatmapRgba(values, 2, 2, -96.f, 0.f, lut, out);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(out[i], lut[0]);
}

TEST(HeatmapRgba, PeakMapsToLut255) {
    uint32_t lut[256];
    buildHeatmapLut(kStops, 8, lut);
    const float values[1] = {0.f};
    uint32_t out[1] = {};
    fillHeatmapRgba(values, 1, 1, -96.f, 0.f, lut, out);
    EXPECT_EQ(out[0], lut[255]);
}

TEST(Spectrogram, RevisionBumpsOnPushAndClear) {
    wa::Spectrogram sg(8, 16, 20.0, 24000.0, 48000);
    EXPECT_EQ(sg.revision(), 0u);
    std::vector<float> col(1025, -96.f);
    sg.pushColumn(col);
    EXPECT_EQ(sg.revision(), 1u);
    sg.pushColumn(col);
    EXPECT_EQ(sg.revision(), 2u);
    sg.clear();
    EXPECT_EQ(sg.revision(), 3u);
}

// Device-free timing for the CPU half of the new submit path (3 heatmaps @ 128x480).
TEST(HeatmapRgba, FillThreeHeatmapsTiming) {
    constexpr int kRows = 128;
    constexpr int kCols = 480;
    constexpr int kN = 3;
    uint32_t lut[256];
    buildHeatmapLut(kStops, 8, lut);

    std::vector<float> values(static_cast<size_t>(kRows * kCols), -40.f);
    std::vector<uint32_t> out(static_cast<size_t>(kRows * kCols));

    // Warmup
    for (int i = 0; i < 4; ++i)
        fillHeatmapRgba(values.data(), kRows, kCols, -96.f, 0.f, lut, out.data());

    constexpr int kIters = 64;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        for (int h = 0; h < kN; ++h)
            fillHeatmapRgba(values.data(), kRows, kCols, -96.f, 0.f, lut, out.data());
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double perFrameNs = ns / kIters;
    std::cout << "slice=heatmap-rgba-fill N=" << kN << " rows=" << kRows << " cols=" << kCols
              << " iters=" << kIters << " nanos_per_frame=" << static_cast<long long>(perFrameNs)
              << "\n";
    // Sanity: should be well under the admitted PlotHeatmap 1.68 ms for 3 heatmaps.
    EXPECT_LT(perFrameNs, 1'680'000.0);
}