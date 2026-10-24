#include <gtest/gtest.h>
#include "PerformanceCalls.h"
#include "CallLogStore.h"

TEST(PerformanceCalls, FixedInputHasTwoSourcesAndFullStructuredParameters) {
    const auto calls = wa::performanceCalls(10000, 2);
    ASSERT_EQ(calls.size(), 10000u);
    EXPECT_EQ(calls[0].pid, 42001u);
    EXPECT_EQ(calls[1].pid, 42002u);
    EXPECT_EQ(calls[0].method, "Initialize");
    ASSERT_EQ(calls[0].parameters.size(), 16u);
    EXPECT_EQ(calls[0].parameters[0].type, "AUDCLNT_SHAREMODE");
    EXPECT_EQ(calls[0].parameters[0].pre, "AUDCLNT_SHAREMODE_SHARED");
    EXPECT_EQ(calls[0].parameters[2].depth, 1u);
    EXPECT_EQ(calls[0].parameters[7].depth, 2u);
    EXPECT_NE(calls[0].parameters[15].post.find("\n"), std::string::npos);
    EXPECT_NE(calls[0].args.find("\n"), std::string::npos);
    EXPECT_NE(calls[0].parameters[1].pre.find("0x"), std::string::npos);
    EXPECT_EQ(calls[0].parameters[14].type, "HRESULT");
    const auto again = wa::performanceCalls(10000, 2);
    EXPECT_EQ(wa::CallLogStore::toJsonLine(calls[8192]), wa::CallLogStore::toJsonLine(again[8192]));
    const auto onePid = wa::performanceCalls(1000, 1);
    ASSERT_EQ(onePid.size(), 1000u);
    EXPECT_EQ(onePid[999].pid, 42001u);
    EXPECT_EQ(onePid[999].parameters.size(), 16u);
}
