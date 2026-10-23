#include "CallLogFilter.h"
#include <gtest/gtest.h>

using wa::call_log_filter::FilterOpts;
using wa::call_log_filter::asciiEqualsIgnoreCase;
using wa::call_log_filter::callMatches;
using wa::call_log_filter::formatCallName;
using wa::call_log_filter::fuzzyMatch;
using wa::call_log_filter::matchingIndices;
using wa::call_log_filter::uniqueIfaces;

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

TEST(CallLogFilter, IfaceFilterExactIgnoreCase) {
    std::vector<wa::HookedCall> calls(3);
    calls[0].iface = "IAudioClient";
    calls[0].method = "Initialize";
    calls[1].iface = "IAudioRenderClient";
    calls[1].method = "GetBuffer";
    calls[2].iface = "IAudioClient";
    calls[2].method = "Start";
    calls[2].hresult = static_cast<int32_t>(0x88890004);  // AUDCLNT_E_DEVICE_INVALIDATED-ish

    FilterOpts opts;
    opts.iface = "iaudioclient";
    auto onlyClient = matchingIndices(calls, opts);
    ASSERT_EQ(onlyClient.size(), 2u);
    EXPECT_EQ(onlyClient[0], 0u);
    EXPECT_EQ(onlyClient[1], 2u);

    opts.iface = "IAudioRenderClient";
    auto onlyRender = matchingIndices(calls, opts);
    ASSERT_EQ(onlyRender.size(), 1u);
    EXPECT_EQ(onlyRender[0], 1u);

    EXPECT_TRUE(asciiEqualsIgnoreCase("IAudioClient", "iaudioclient"));
    EXPECT_FALSE(asciiEqualsIgnoreCase("IAudioClient", "IAudio"));
}

TEST(CallLogFilter, FailuresOnlyExcludesSOk) {
    std::vector<wa::HookedCall> calls(3);
    calls[0].iface = "IAudioClient";
    calls[0].method = "Initialize";
    calls[0].hresult = 0;  // S_OK
    calls[1].iface = "IAudioClient";
    calls[1].method = "Start";
    calls[1].hresult = static_cast<int32_t>(0x80004005);  // E_FAIL
    calls[2].iface = "IAudioRenderClient";
    calls[2].method = "GetBuffer";
    calls[2].hresult = 1;  // S_FALSE / non-zero

    FilterOpts opts;
    opts.failuresOnly = true;
    auto fails = matchingIndices(calls, opts);
    ASSERT_EQ(fails.size(), 2u);
    EXPECT_EQ(fails[0], 1u);
    EXPECT_EQ(fails[1], 2u);

    wa::HookedCall ok = calls[0];
    EXPECT_TRUE(callMatches(ok, FilterOpts{}));
    FilterOpts failOnly;
    failOnly.failuresOnly = true;
    EXPECT_FALSE(callMatches(ok, failOnly));
    EXPECT_TRUE(callMatches(calls[1], failOnly));
}

TEST(CallLogFilter, CombinedTextIfaceFailures) {
    std::vector<wa::HookedCall> calls(4);
    calls[0].iface = "IAudioClient";
    calls[0].method = "Initialize";
    calls[0].args = "share=shared";
    calls[0].hresult = 0;
    calls[1].iface = "IAudioClient";
    calls[1].method = "Initialize";
    calls[1].args = "share=exclusive";
    calls[1].hresult = static_cast<int32_t>(0x88890008);
    calls[2].iface = "IAudioRenderClient";
    calls[2].method = "GetBuffer";
    calls[2].hresult = static_cast<int32_t>(0x80004005);
    calls[3].iface = "IAudioClient";
    calls[3].method = "Start";
    calls[3].hresult = static_cast<int32_t>(0x80004005);

    FilterOpts opts;
    opts.text = "init";
    opts.iface = "IAudioClient";
    opts.failuresOnly = true;
    auto hit = matchingIndices(calls, opts);
    ASSERT_EQ(hit.size(), 1u);
    EXPECT_EQ(hit[0], 1u);
}

TEST(CallLogFilter, UniqueIfacesSorted) {
    std::vector<wa::HookedCall> calls(4);
    calls[0].iface = "IAudioRenderClient";
    calls[1].iface = "IAudioClient";
    calls[2].iface = "iaudioclient";  // duplicate ignore-case
    calls[3].iface = "IAudioCaptureClient";
    const auto ifaces = uniqueIfaces(calls);
    ASSERT_EQ(ifaces.size(), 3u);
    EXPECT_EQ(ifaces[0], "IAudioCaptureClient");
    EXPECT_EQ(ifaces[1], "IAudioClient");
    EXPECT_EQ(ifaces[2], "IAudioRenderClient");
}
