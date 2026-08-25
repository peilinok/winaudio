#include <gtest/gtest.h>
#include "OnDemandAttach.h"

using namespace wa;

TEST(OnDemandAttach, PidZeroFailsClosed) {
    EXPECT_EQ(evaluateAttach(0, 100, true, true, "chrome.exe"), AttachBlock::PidZero);
}

TEST(OnDemandAttach, SelfProcessFailsClosed) {
    EXPECT_EQ(evaluateAttach(100, 100, true, true, "WinAudioGui.exe"), AttachBlock::SelfProcess);
}

TEST(OnDemandAttach, AudiodgFailsClosed) {
    EXPECT_EQ(evaluateAttach(8, 100, true, true, "audiodg.exe"), AttachBlock::Audiodg);
    EXPECT_EQ(evaluateAttach(8, 100, true, true, "AUDIODG.EXE"), AttachBlock::Audiodg);
}

TEST(OnDemandAttach, CrossBitnessFailsClosed) {
    EXPECT_EQ(evaluateAttach(4242, 100, false, true, "chrome.exe"), AttachBlock::CrossBitness);
    EXPECT_STREQ(attachBlockText(AttachBlock::CrossBitness), "Attach failed: cross-bitness");
}

TEST(OnDemandAttach, MissingDebugRightsFailsClosed) {
    EXPECT_EQ(evaluateAttach(4242, 100, true, false, "chrome.exe"), AttachBlock::NoDebugRights);
    EXPECT_STREQ(attachBlockText(AttachBlock::NoDebugRights),
                 "Attach failed: missing debug rights");
}

TEST(OnDemandAttach, SameBitnessWithDebugIsAllowed) {
    EXPECT_EQ(evaluateAttach(4242, 100, true, true, "chrome.exe"), AttachBlock::None);
}

TEST(OnDemandAttach, DoesNotLaunchSuspendedOrAutoInject) {
    // Policy has no launch-suspended or broadcast-inject mode; start() is on-demand
    // for one PID. This test locks the public flags.
    EXPECT_FALSE(kAttachLaunchSuspended);
    EXPECT_FALSE(kAttachAutoInject);
}
