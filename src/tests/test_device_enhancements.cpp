#include <gtest/gtest.h>
#include "IAudioBackend.h"

using namespace wa;

TEST(DeviceEnhancements, StateName) {
    EXPECT_STREQ(enhancementsStateName(EnhancementsState::Unknown), "unknown");
    EXPECT_STREQ(enhancementsStateName(EnhancementsState::Enabled), "on");
    EXPECT_STREQ(enhancementsStateName(EnhancementsState::Disabled), "off");
}

TEST(DeviceEnhancements, FromSysFxUlVal) {
    EXPECT_EQ(enhancementsStateFromSysFxUlVal(0), EnhancementsState::Enabled);   // ENDPOINT_SYSFX_ENABLED
    EXPECT_EQ(enhancementsStateFromSysFxUlVal(1), EnhancementsState::Disabled);  // ENDPOINT_SYSFX_DISABLED
    EXPECT_EQ(enhancementsStateFromSysFxUlVal(2), EnhancementsState::Disabled);
}

TEST(DeviceEnhancements, DeviceInfoDefaultsUnknown) {
    DeviceInfo info{};
    EXPECT_EQ(info.enhancements, EnhancementsState::Unknown);
    EXPECT_TRUE(info.enhancementOptions.empty());
}

TEST(DeviceEnhancements, OptionsCarryDescriptors) {
    DeviceInfo info{};
    info.enhancements = EnhancementsState::Enabled;
    info.enhancementOptions.push_back(EnhancementOption{"SFX", "Stream effect (SFX)", "{clsid}"});
    info.enhancementOptions.push_back(EnhancementOption{"MFX", "Mode effect (MFX)", "{mfx}"});
    ASSERT_EQ(info.enhancementOptions.size(), 2u);
    EXPECT_EQ(info.enhancementOptions[0].id, "SFX");
    EXPECT_EQ(info.enhancementOptions[1].value, "{mfx}");
    EXPECT_STREQ(enhancementsStateName(info.enhancements), "on");
}