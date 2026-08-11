#include <gtest/gtest.h>
#include "ChartHost.h"

using namespace wa::chart_host;

TEST(ChartHostResolvePanelIds, CaptureOnlyAlwaysCapture) {
    const auto ids = resolvePanelIds(Mode::CaptureOnly, {1, 0, 2});
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], kPanelCapture);
}

TEST(ChartHostResolvePanelIds, CaptureOnlyIgnoresEmptyOrder) {
    const auto ids = resolvePanelIds(Mode::CaptureOnly, {});
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], kPanelCapture);
}

TEST(ChartHostResolvePanelIds, DualDefaultWhenEmpty) {
    const auto ids = resolvePanelIds(Mode::DualReorderable, {});
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], kPanelCapture);
    EXPECT_EQ(ids[1], kPanelRender);
}

TEST(ChartHostResolvePanelIds, DualPreservesSwappedOrder) {
    const auto ids = resolvePanelIds(Mode::DualReorderable, {1, 0});
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], kPanelRender);
    EXPECT_EQ(ids[1], kPanelCapture);
}

TEST(ChartHostResolvePanelIds, DualDropsDuplicatesKeepsFirst) {
    const auto ids = resolvePanelIds(Mode::DualReorderable, {0, 0, 1, 1});
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], kPanelCapture);
    EXPECT_EQ(ids[1], kPanelRender);
}

TEST(ChartHostResolvePanelIds, DualIgnoresUnknownAndFillsMissing) {
    const auto ids = resolvePanelIds(Mode::DualReorderable, {2, 1, 99});
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], kPanelRender);
    EXPECT_EQ(ids[1], kPanelCapture); // missing capture appended after known
}

TEST(ChartHostResolvePanelIds, DualOnlyCaptureGetsRenderAppended) {
    const auto ids = resolvePanelIds(Mode::DualReorderable, {0});
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], kPanelCapture);
    EXPECT_EQ(ids[1], kPanelRender);
}
