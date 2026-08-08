#include <gtest/gtest.h>
#include "ChartsTimeZoomPolicy.h"

using namespace wa::charts_time_zoom;

TEST(ChartsTimeZoomPolicy, MinWidthIsMaxOfTenMsAndHopCappedByHistory) {
    EXPECT_DOUBLE_EQ(minWidthSeconds(/*hop=*/0.005, /*H=*/10.0), 0.010);
    EXPECT_DOUBLE_EQ(minWidthSeconds(/*hop=*/0.021333, /*H=*/10.0), 0.021333);
    EXPECT_DOUBLE_EQ(minWidthSeconds(/*hop=*/0.5, /*H=*/0.1), 0.1);
    EXPECT_DOUBLE_EQ(minWidthSeconds(/*hop=*/0.01, /*H=*/0.0), 0.0);
}

TEST(ChartsTimeZoomPolicy, FullHistoryIsZeroToH) {
    const Interval full = fullHistory(10.24);
    EXPECT_DOUBLE_EQ(full.x0, 0.0);
    EXPECT_DOUBLE_EQ(full.x1, 10.24);
    EXPECT_DOUBLE_EQ(fullHistory(0.0).x1, 0.0);
}

TEST(ChartsTimeZoomPolicy, ZoomInHalvesWidthAboutCenter) {
    // [2, 6] width 4, center 4 -> half width 2 -> [3, 5]
    const Interval z = zoomCentered(2.0, 6.0, /*H=*/10.0, /*hop=*/0.01, /*factor=*/0.5);
    EXPECT_NEAR(z.x0, 3.0, 1e-12);
    EXPECT_NEAR(z.x1, 5.0, 1e-12);
    EXPECT_NEAR(z.x1 - z.x0, 2.0, 1e-12);
}

TEST(ChartsTimeZoomPolicy, ZoomOutDoublesWidthAboutCenter) {
    // [3, 5] width 2, center 4 -> width 4 -> [2, 6]
    const Interval z = zoomCentered(3.0, 5.0, /*H=*/10.0, /*hop=*/0.01, /*factor=*/2.0);
    EXPECT_NEAR(z.x0, 2.0, 1e-12);
    EXPECT_NEAR(z.x1, 6.0, 1e-12);
}

TEST(ChartsTimeZoomPolicy, ZoomOutClampsToFullHistory) {
    const Interval z = zoomCentered(1.0, 9.0, /*H=*/10.0, /*hop=*/0.01, /*factor=*/2.0);
    EXPECT_NEAR(z.x0, 0.0, 1e-12);
    EXPECT_NEAR(z.x1, 10.0, 1e-12);
}

TEST(ChartsTimeZoomPolicy, ZoomInStopsAtMinWidth) {
    const double hop = 0.02;
    const double H = 10.0;
    const double minW = minWidthSeconds(hop, H);
    // Already at min width around center.
    const double c = 5.0;
    const Interval z = zoomCentered(c - 0.5 * minW, c + 0.5 * minW, H, hop, 0.5);
    EXPECT_NEAR(z.x1 - z.x0, minW, 1e-12);
    EXPECT_FALSE(canZoomIn(z.x0, z.x1, H, hop));
}

TEST(ChartsTimeZoomPolicy, CanZoomOutFalseAtFullHistory) {
    EXPECT_FALSE(canZoomOut(0.0, 10.0, 10.0));
    EXPECT_TRUE(canZoomOut(1.0, 9.0, 10.0));
}

TEST(ChartsTimeZoomPolicy, CanZoomInFalseAtMinWidth) {
    const double hop = 0.01;
    const double H = 5.0;
    const double minW = minWidthSeconds(hop, H);
    EXPECT_FALSE(canZoomIn(0.0, minW, H, hop));
    EXPECT_TRUE(canZoomIn(0.0, minW * 4.0, H, hop));
}

TEST(ChartsTimeZoomPolicy, EdgeClampDoesNotInvertInterval) {
    // Near left edge: zoom out should stay ordered and inside [0, H].
    const Interval z = zoomCentered(0.0, 0.5, /*H=*/10.0, /*hop=*/0.01, /*factor=*/2.0);
    EXPECT_LE(z.x0, z.x1);
    EXPECT_GE(z.x0, 0.0);
    EXPECT_LE(z.x1, 10.0);
    EXPECT_NEAR(z.x1 - z.x0, 1.0, 1e-12);
}

TEST(ChartsTimeZoomPolicy, ClampToHistoryKeepsValidRange) {
    const Interval c = clampToHistory(-1.0, 3.0, 10.0);
    EXPECT_GE(c.x0, 0.0);
    EXPECT_LE(c.x1, 10.0);
    EXPECT_LE(c.x0, c.x1);
}
