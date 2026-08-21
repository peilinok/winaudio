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
    a.sharedOk = true;
    wa::FormatSupport b{};
    b.fmt = wa::AudioFormat{96000, 2, 24, false};
    b.exclusiveOk = true;
    recipe.caps.matrix = {a, b};
    const auto ok = wa::create_recipe::sharedCandidates(recipe.caps);
    ASSERT_EQ(ok.size(), 1u);
    EXPECT_EQ(ok[0], a.fmt);
}
