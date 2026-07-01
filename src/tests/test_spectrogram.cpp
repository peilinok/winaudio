#include <gtest/gtest.h>
#include <vector>
#include "Spectrogram.h"

// Parameters used throughout: sr=48000, N=2048 => 1025 bins,
// fmin=20, fmax=24000, rows=128, cols=200, floor=-96.
static const int   kRows  = 128;
static const int   kCols  = 200;
static const float kFloor = -96.f;

static wa::Spectrogram makeSpec() {
    return wa::Spectrogram(kRows, kCols, 20.0, 24000.0, 48000, kFloor);
}

static std::vector<float> flatColumn(float value, int bins = 1025) {
    return std::vector<float>(bins, value);
}

// ---- DimsAndEmpty ----------------------------------------------------------
TEST(Spectrogram, DimsAndEmpty) {
    auto sg = makeSpec();
    EXPECT_EQ(sg.rows(), kRows);
    EXPECT_EQ(sg.cols(), kCols);
    const float* d = sg.data();
    for (int i = 0; i < kRows * kCols; ++i)
        EXPECT_FLOAT_EQ(d[i], kFloor) << "cell " << i << " should be floor before any push";

    // Verify clear() restores floor after a push
    std::vector<float> col = flatColumn(-10.f);
    sg.pushColumn(col);
    sg.clear();
    d = sg.data();
    for (int i = 0; i < kRows * kCols; ++i)
        EXPECT_FLOAT_EQ(d[i], kFloor) << "cell " << i << " should be floor after clear()";
}

// ---- HighFreqSpikeToTopRow -------------------------------------------------
// bin 1024 = 24000 Hz = fmax => must land in row 0 (highest freq = top).
TEST(Spectrogram, HighFreqSpikeToTopRow) {
    auto sg = makeSpec();
    std::vector<float> col(1025, kFloor);
    col[1024] = 0.f;  // spike at Nyquist
    sg.pushColumn(col);

    const float* d = sg.data();
    // Top row, newest col
    EXPECT_NEAR(d[0 * kCols + (kCols - 1)], 0.f, 0.01f)
        << "Nyquist spike must appear in top row (row 0)";
    // Bottom row, newest col must stay at floor
    EXPECT_FLOAT_EQ(d[(kRows - 1) * kCols + (kCols - 1)], kFloor)
        << "Bottom row must not capture high-freq spike";
}

// ---- LowFreqSpikeToBottomRow -----------------------------------------------
// bin 1 = ~23 Hz, near fmin => must land in the bottom row (lowest freq = bottom).
TEST(Spectrogram, LowFreqSpikeToBottomRow) {
    auto sg = makeSpec();
    std::vector<float> col(1025, kFloor);
    col[1] = 0.f;  // spike near DC/fmin
    sg.pushColumn(col);

    const float* d = sg.data();
    // Bottom row (row rows-1), newest col
    EXPECT_NEAR(d[(kRows - 1) * kCols + (kCols - 1)], 0.f, 0.01f)
        << "Near-DC spike must appear in bottom row (row rows-1)";
    // Top row, newest col must stay at floor
    EXPECT_FLOAT_EQ(d[0 * kCols + (kCols - 1)], kFloor)
        << "Top row must not capture low-freq spike";
}

// ---- ScrollNewestAtRight ---------------------------------------------------
// Newest column is always at cols-1; older columns scroll left.
TEST(Spectrogram, ScrollNewestAtRight) {
    auto sg = makeSpec();

    // Push -10 (all bins)
    sg.pushColumn(flatColumn(-10.f));
    // Push -30 (all bins)
    sg.pushColumn(flatColumn(-30.f));

    const float* d = sg.data();
    for (int r = 0; r < kRows; ++r) {
        EXPECT_NEAR(d[r * kCols + (kCols - 1)], -30.f, 0.01f)
            << "row " << r << ": newest col should be -30 after second push";
        EXPECT_NEAR(d[r * kCols + (kCols - 2)], -10.f, 0.01f)
            << "row " << r << ": col cols-2 should be -10 (scrolled one left)";
    }

    // After cols more pushes of floor, the -10/-30 values must have scrolled off
    for (int i = 0; i < kCols; ++i)
        sg.pushColumn(flatColumn(kFloor));
    d = sg.data();
    for (int i = 0; i < kRows * kCols; ++i)
        EXPECT_FLOAT_EQ(d[i], kFloor)
            << "All values should be floor after scrolling -10/-30 off the buffer";
}

// ---- ReductionIsMax --------------------------------------------------------
// Two bins inside one row's band: the row shows the MAX of the two.
// The top row's band covers roughly [969,1024]; we use bins 1000 and 1020.
TEST(Spectrogram, ReductionIsMax) {
    auto sg = makeSpec();
    std::vector<float> col(1025, kFloor);
    col[1000] = -40.f;
    col[1020] = -8.f;
    sg.pushColumn(col);

    const float* d = sg.data();
    // Top row (row 0) should capture the max (-8, not -40)
    EXPECT_NEAR(d[0 * kCols + (kCols - 1)], -8.f, 0.01f)
        << "Top row should show MAX of two bins in its band";
}
