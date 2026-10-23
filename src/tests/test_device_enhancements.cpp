#include <gtest/gtest.h>
#include "IAudioBackend.h"
#include "EndpointEnhancements.h"
#include "Elevate.h"

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

TEST(DeviceEnhancements, ToSysFxUlVal) {
    uint32_t v = 99;
    EXPECT_TRUE(enhancementsStateToSysFxUlVal(EnhancementsState::Enabled, v));
    EXPECT_EQ(v, 0u);
    EXPECT_TRUE(enhancementsStateToSysFxUlVal(EnhancementsState::Disabled, v));
    EXPECT_EQ(v, 1u);
    EXPECT_FALSE(enhancementsStateToSysFxUlVal(EnhancementsState::Unknown, v));
}

TEST(DeviceEnhancements, StateFromName) {
    EXPECT_EQ(enhancementsStateFromName("on"), EnhancementsState::Enabled);
    EXPECT_EQ(enhancementsStateFromName("ON"), EnhancementsState::Enabled);
    EXPECT_EQ(enhancementsStateFromName("off"), EnhancementsState::Disabled);
    EXPECT_EQ(enhancementsStateFromName("Off"), EnhancementsState::Disabled);
    EXPECT_EQ(enhancementsStateFromName("maybe"), EnhancementsState::Unknown);
    EXPECT_EQ(enhancementsStateFromName(""), EnhancementsState::Unknown);
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

TEST(DeviceEnhancements, RefuseWhileStreamingMessage) {
    Result r = refuseEnhancementsWhileStreaming();
    EXPECT_FALSE(r);
    EXPECT_EQ(r.message, "start stream 前才能改");
}

TEST(DeviceEnhancements, SetUnknownStateFailsWithoutDevice) {
    // Unknown is rejected before any COM open.
    Result r = setEndpointEnhancements(DataFlow::Render, L"", EnhancementsState::Unknown);
    EXPECT_FALSE(r);
    EXPECT_NE(r.message.find("on or off"), std::string::npos);
}

TEST(Elevate, ResultIsAccessDeniedRecognizesHresults) {
    EXPECT_FALSE(resultIsAccessDenied(Result::Ok()));
    EXPECT_TRUE(resultIsAccessDenied(
        Result::Fail(static_cast<long>(0x80070005L), "denied")));  // E_ACCESSDENIED
    EXPECT_TRUE(resultIsAccessDenied(Result::Fail(5, "denied")));  // ERROR_ACCESS_DENIED
    EXPECT_FALSE(resultIsAccessDenied(Result::Fail(-1, "other")));
}

TEST(DeviceEnhancements, GateBlocksWhileNotedActive) {
    EXPECT_FALSE(anyAppStreamActive());
    noteAppStreamStarted();
    EXPECT_TRUE(anyAppStreamActive());
    Result r = setEndpointEnhancements(DataFlow::Render, L"", EnhancementsState::Enabled);
    EXPECT_FALSE(r);
    EXPECT_EQ(r.message, "start stream 前才能改");
    noteAppStreamStopped();
    EXPECT_FALSE(anyAppStreamActive());
}
