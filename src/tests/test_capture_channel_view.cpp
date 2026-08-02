#include <gtest/gtest.h>
#include "CaptureChannelView.h"

using namespace wa::capture_channel_view;

TEST(CaptureChannelView, UnknownOrMonoKeepsSingleLegacyPanel) {
    const Plan unknown = makePlan(0);
    EXPECT_EQ(unknown.actualChannels, 1u);
    EXPECT_EQ(unknown.visibleChannels, 1u);
    EXPECT_FALSE(unknown.split);
    EXPECT_FALSE(unknown.truncated);

    const Plan mono = makePlan(1);
    EXPECT_EQ(mono.actualChannels, 1u);
    EXPECT_EQ(mono.visibleChannels, 1u);
    EXPECT_FALSE(mono.split);
    EXPECT_FALSE(mono.truncated);
}

TEST(CaptureChannelView, StereoSplitsWithoutTruncation) {
    const Plan stereo = makePlan(2);
    EXPECT_EQ(stereo.actualChannels, 2u);
    EXPECT_EQ(stereo.visibleChannels, 2u);
    EXPECT_TRUE(stereo.split);
    EXPECT_FALSE(stereo.truncated);
}

TEST(CaptureChannelView, MoreThanEightCapsVisibleChannels) {
    const Plan many = makePlan(9);
    EXPECT_EQ(many.actualChannels, 9u);
    EXPECT_EQ(many.visibleChannels, 8u);
    EXPECT_TRUE(many.split);
    EXPECT_TRUE(many.truncated);
}
