#include "CallLogFilter.h"
#include <gtest/gtest.h>

using wa::call_log_filter::callMatches;
using wa::call_log_filter::formatCallName;
using wa::call_log_filter::fuzzyMatch;
using wa::call_log_filter::matchingIndices;

TEST(CallLogFilter, EmptyFilterMatchesAll) {
    EXPECT_TRUE(fuzzyMatch("IAudioClient::Initialize", ""));
    wa::HookedCall c;
    c.iface = "IAudioClient";
    c.method = "Initialize";
    c.args = "share=shared";
    EXPECT_TRUE(callMatches(c, ""));
}

TEST(CallLogFilter, FuzzySubsequenceOnCallName) {
    EXPECT_TRUE(fuzzyMatch("IAudioClient::Initialize", "iacl"));
    EXPECT_TRUE(fuzzyMatch("IAudioClient::Initialize", "init"));
    EXPECT_TRUE(fuzzyMatch("IAudioClient::Initialize", "IAudio Initialize"));
    EXPECT_FALSE(fuzzyMatch("IAudioClient::Initialize", "xyz"));
    EXPECT_FALSE(fuzzyMatch("IAudioClient::Initialize", "InitializeZ"));
}

TEST(CallLogFilter, MatchesArgsOrCall) {
    wa::HookedCall c;
    c.iface = "IAudioClient";
    c.method = "Initialize";
    c.args = "share=shared rate=48000";
    EXPECT_EQ(formatCallName(c), "IAudioClient::Initialize");
    EXPECT_TRUE(callMatches(c, "48000"));
    EXPECT_TRUE(callMatches(c, "iaudio"));
    EXPECT_FALSE(callMatches(c, "nofit"));
}

TEST(CallLogFilter, MatchingIndicesOrder) {
    std::vector<wa::HookedCall> calls(3);
    calls[0].iface = "IAudioClient";
    calls[0].method = "Initialize";
    calls[1].iface = "IAudioRenderClient";
    calls[1].method = "GetBuffer";
    calls[1].args = "frames=128";
    calls[2].iface = "IAudioClient";
    calls[2].method = "Start";
    const auto all = matchingIndices(calls, "");
    ASSERT_EQ(all.size(), 3u);
    const auto onlyInit = matchingIndices(calls, "Initialize");
    ASSERT_EQ(onlyInit.size(), 1u);
    EXPECT_EQ(onlyInit[0], 0u);
    const auto audio = matchingIndices(calls, "iaudio");
    ASSERT_EQ(audio.size(), 3u);
}
