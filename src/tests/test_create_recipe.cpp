#include <gtest/gtest.h>
#include "CreateRecipe.h"

TEST(CreateRecipe, DefaultHasNoRequestedFormat) {
    wa::create_recipe::FormatState st;
    EXPECT_FALSE(st.haveRequested);
    EXPECT_EQ(st.choiceIdx, 0);
    EXPECT_EQ(wa::create_recipe::requestedOrNull(st), nullptr);
}

TEST(CreateRecipe, SelectDefaultClearsRequestedAndKeepsMixForDisplay) {
    wa::create_recipe::FormatState st;
    wa::AudioFormat mix{48000, 2, 32, true};
    st.haveRequested = true;
    st.choiceIdx = 3;
    wa::create_recipe::selectDefault(st, mix);
    EXPECT_FALSE(st.haveRequested);
    EXPECT_EQ(st.choiceIdx, 0);
    EXPECT_EQ(st.selected, mix);
    EXPECT_EQ(wa::create_recipe::requestedOrNull(st), nullptr);
}

TEST(CreateRecipe, SelectCandidateSetsRequested) {
    wa::create_recipe::FormatState st;
    wa::AudioFormat fmt{44100, 2, 16, false};
    wa::create_recipe::selectCandidate(st, fmt, 2);
    EXPECT_TRUE(st.haveRequested);
    EXPECT_EQ(st.choiceIdx, 2);
    EXPECT_EQ(st.selected, fmt);
    ASSERT_NE(wa::create_recipe::requestedOrNull(st), nullptr);
    EXPECT_EQ(*wa::create_recipe::requestedOrNull(st), fmt);
}

TEST(CreateRecipe, ApplyCustomSetsRequested) {
    wa::create_recipe::FormatState st;
    std::string err;
    ASSERT_TRUE(wa::create_recipe::applyCustom(st, "96000/24/2f", &err));
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(st.haveRequested);
    EXPECT_EQ(st.choiceIdx, -1);
    EXPECT_EQ(st.selected.sampleRate, 96000u);
    EXPECT_EQ(st.selected.bitsPerSample, 24);
    EXPECT_EQ(st.selected.channels, 2);
    EXPECT_TRUE(st.selected.isFloat);
}

TEST(CreateRecipe, ApplyCustomRejectsInvalidWithoutChangingState) {
    wa::create_recipe::FormatState st;
    wa::create_recipe::selectCandidate(st, wa::AudioFormat{48000, 2, 16, false}, 1);
    const wa::create_recipe::FormatState before = st;
    std::string err;
    EXPECT_FALSE(wa::create_recipe::applyCustom(st, "not-a-format", &err));
    EXPECT_EQ(err, "invalid format");
    EXPECT_EQ(st.haveRequested, before.haveRequested);
    EXPECT_EQ(st.choiceIdx, before.choiceIdx);
    EXPECT_EQ(st.selected, before.selected);
}

TEST(CreateRecipe, SharedCandidatesKeepsOnlySharedOk) {
    wa::create_recipe::CreateRecipe recipe;
    wa::FormatSupport a{};
    a.fmt = wa::AudioFormat{48000, 2, 16, false};
    a.shared = wa::SupportLevel::Exact;
    wa::FormatSupport b{};
    b.fmt = wa::AudioFormat{96000, 2, 24, false};
    b.exclusive = wa::SupportLevel::Exact;
    recipe.caps.matrix = {a, b};
    const auto ok = wa::create_recipe::sharedCandidates(recipe.caps);
    ASSERT_EQ(ok.size(), 1u);
    EXPECT_EQ(ok[0], a.fmt);
}

TEST(CreateRecipe, SharedFormatChoicesPrioritizeSourcesAndLimitRecommendationsToMixRate) {
    wa::DeviceCapabilities caps;
    caps.hasMix = true;
    caps.mixFormat = wa::AudioFormat{48000, 8, 32, true};

    wa::FormatSupport mix{};
    mix.fmt = caps.mixFormat;
    mix.origins = wa::formatOriginBit(wa::FormatOrigin::Mix);
    mix.shared = wa::SupportLevel::Exact;
    wa::FormatSupport device{};
    device.fmt = wa::AudioFormat{44100, 8, 16, false};
    device.origins = wa::formatOriginBit(wa::FormatOrigin::Device);
    device.shared = wa::SupportLevel::ClosestMatch;
    wa::FormatSupport recommended{};
    recommended.fmt = wa::AudioFormat{48000, 8, 16, false};
    recommended.origins = wa::formatOriginBit(wa::FormatOrigin::Standard);
    recommended.shared = wa::SupportLevel::ClosestMatch;
    wa::FormatSupport otherRate = recommended;
    otherRate.fmt.sampleRate = 96000;
    wa::FormatSupport unsupportedSource = device;
    unsupportedSource.fmt.bitsPerSample = 24;
    unsupportedSource.origins = wa::formatOriginBit(wa::FormatOrigin::Oem);
    unsupportedSource.shared = wa::SupportLevel::Unsupported;
    caps.matrix = {mix, device, recommended, otherRate, unsupportedSource};

    const auto choices = wa::create_recipe::sharedFormatChoices(caps);

    ASSERT_EQ(choices.size(), 3u);
    EXPECT_EQ(choices[0].fmt, mix.fmt);
    EXPECT_EQ(choices[1].fmt, device.fmt);
    EXPECT_EQ(choices[2].fmt, recommended.fmt);
    EXPECT_TRUE(wa::hasFormatOrigin(choices[0].origins, wa::FormatOrigin::Mix));
    EXPECT_EQ(choices[1].shared, wa::SupportLevel::ClosestMatch);
}

TEST(CreateRecipe, ChangedFormatDeviceResetsToAutomaticButRefreshKeepsSelection) {
    wa::create_recipe::CreateRecipe recipe;
    wa::create_recipe::selectCandidate(
        recipe.format, wa::AudioFormat{96000, 2, 24, false}, 2);
    wa::DeviceCapabilities firstCaps;
    firstCaps.hasMix = true;
    firstCaps.mixFormat = wa::AudioFormat{48000, 8, 32, true};

    EXPECT_TRUE(wa::create_recipe::updateFormatDevice(
        recipe, 1, L"g431", firstCaps, firstCaps.mixFormat));
    EXPECT_FALSE(recipe.format.haveRequested);
    EXPECT_EQ(recipe.format.choiceIdx, 0);
    EXPECT_EQ(recipe.format.selected, firstCaps.mixFormat);

    wa::create_recipe::selectCandidate(
        recipe.format, wa::AudioFormat{48000, 8, 16, false}, 1);
    wa::DeviceCapabilities refreshedCaps = firstCaps;
    recipe.deviceShown = -1; // capabilities cache invalidated by a device-list refresh
    EXPECT_FALSE(wa::create_recipe::updateFormatDevice(
        recipe, 1, L"g431", refreshedCaps, firstCaps.mixFormat));
    EXPECT_TRUE(recipe.format.haveRequested);
    EXPECT_EQ(recipe.format.selected, (wa::AudioFormat{48000, 8, 16, false}));

    recipe.deviceShown = 0; // Application Loopback: None
    recipe.deviceId.clear();
    wa::create_recipe::selectCandidate(
        recipe.format, wa::AudioFormat{48000, 8, 16, false}, 1);
    EXPECT_TRUE(wa::create_recipe::updateFormatDevice(
        recipe, 1, L"", refreshedCaps, wa::AudioFormat{}));
    EXPECT_FALSE(recipe.format.haveRequested); // Default render reference is a real change
}
