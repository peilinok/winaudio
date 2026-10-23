#include "PipelineSplit.h"
#include <gtest/gtest.h>

using wa::pipeline_split::firstPaneSize;
using wa::pipeline_split::kDefaultCallTableFrac;
using wa::pipeline_split::kDefaultSessionsFrac;
using wa::pipeline_split::kDefaultTopRowFrac;
using wa::pipeline_split::nudgeFrac;

TEST(PipelineSplit, DefaultsMatchPriorLayout) {
    EXPECT_FLOAT_EQ(kDefaultSessionsFrac, 0.68f);
    EXPECT_FLOAT_EQ(kDefaultCallTableFrac, 0.40f);
    EXPECT_FLOAT_EQ(kDefaultTopRowFrac, 0.52f);
}

TEST(PipelineSplit, NudgeFracClampsAndScales) {
    EXPECT_FLOAT_EQ(nudgeFrac(0.5f, 0.0f, 100.0f, 0.2f, 0.8f), 0.5f);
    EXPECT_NEAR(nudgeFrac(0.5f, 10.0f, 100.0f, 0.2f, 0.8f), 0.6f, 1e-5f);
    EXPECT_FLOAT_EQ(nudgeFrac(0.5f, 1000.0f, 100.0f, 0.2f, 0.8f), 0.8f);
    EXPECT_FLOAT_EQ(nudgeFrac(0.5f, -1000.0f, 100.0f, 0.2f, 0.8f), 0.2f);
    EXPECT_FLOAT_EQ(nudgeFrac(0.5f, 10.0f, 0.0f, 0.2f, 0.8f), 0.5f);
}

TEST(PipelineSplit, FirstPaneRespectsMins) {
    EXPECT_NEAR(firstPaneSize(1000.0f, 0.40f, 160.0f, 200.0f), 400.0f, 1e-3f);
    // Too small frac still respects minFirst.
    EXPECT_NEAR(firstPaneSize(1000.0f, 0.05f, 160.0f, 200.0f), 160.0f, 1e-3f);
    // Too large frac still leaves minSecond.
    EXPECT_NEAR(firstPaneSize(1000.0f, 0.95f, 160.0f, 200.0f), 800.0f, 1e-3f);
}

TEST(PipelineSplit, CallTableDefaultIsFourSix) {
    const float usable = 1000.0f;
    const float table = firstPaneSize(usable, kDefaultCallTableFrac, 160.0f, 200.0f);
    EXPECT_NEAR(table / usable, 0.40f, 1e-5f);
    EXPECT_NEAR((usable - table) / usable, 0.60f, 1e-5f);
}
