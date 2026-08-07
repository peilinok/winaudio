#include <gtest/gtest.h>
#include "ChartsFreezePolicy.h"

using namespace wa::charts_freeze;

TEST(ChartsFreezePolicy, RefreshOnlyWhenRunningAndNotFrozen) {
    EXPECT_TRUE(shouldRefreshCharts(/*overallRunning=*/true, /*frozen=*/false));
    EXPECT_FALSE(shouldRefreshCharts(/*overallRunning=*/true, /*frozen=*/true));
    EXPECT_FALSE(shouldRefreshCharts(/*overallRunning=*/false, /*frozen=*/false));
    EXPECT_FALSE(shouldRefreshCharts(/*overallRunning=*/false, /*frozen=*/true));
}

TEST(ChartsFreezePolicy, ControlEnabledOnlyWhenRunning) {
    EXPECT_TRUE(isControlEnabled(/*overallRunning=*/true));
    EXPECT_FALSE(isControlEnabled(/*overallRunning=*/false));
}

TEST(ChartsFreezePolicy, ClearsFreezeWhenNotRunning) {
    EXPECT_TRUE(shouldClearFreeze(/*overallRunning=*/false));
    EXPECT_FALSE(shouldClearFreeze(/*overallRunning=*/true));
}

TEST(ChartsFreezePolicy, ApplyLifecycleClearsFreezeWhenNotRunning) {
    EXPECT_FALSE(applyLifecycle(/*overallRunning=*/false, /*frozen=*/true));
    EXPECT_FALSE(applyLifecycle(/*overallRunning=*/false, /*frozen=*/false));
    EXPECT_TRUE(applyLifecycle(/*overallRunning=*/true, /*frozen=*/true));
    EXPECT_FALSE(applyLifecycle(/*overallRunning=*/true, /*frozen=*/false));
}

TEST(ChartsFreezePolicy, ResetRenderVisualsSuppressedWhileFrozen) {
    EXPECT_FALSE(shouldResetRenderVisuals(/*frozen=*/true));
    EXPECT_TRUE(shouldResetRenderVisuals(/*frozen=*/false));
}

TEST(ChartsFreezePolicy, ClearedSessionStartsUnfrozen) {
    // New / reset session: frozen flag is false; lifecycle while idle keeps it false.
    constexpr bool kNewSessionFrozen = false;
    EXPECT_FALSE(kNewSessionFrozen);
    EXPECT_FALSE(applyLifecycle(/*overallRunning=*/false, kNewSessionFrozen));
}
