#include <gtest/gtest.h>
#include "HookedCall.h"
#include "HookedCallPod.h"
#include <cstring>
#include <string>
#include <vector>

using namespace wa;
using namespace wa::hook_ipc;

TEST(CallArgBlob, RoundTripNestedWaveFields) {
    uint8_t blob[kArgBlobCap] = {};
    ArgBlobWriter w;
    w.reset(blob, kArgBlobCap);
    ASSERT_TRUE(w.push(0, kArgIn, "AUDCLNT_SHAREMODE", "ShareMode", "AUDCLNT_SHAREMODE_SHARED",
                       "AUDCLNT_SHAREMODE_SHARED"));
    ASSERT_TRUE(w.push(0, kArgIn, "WAVEFORMATEX*", "pFormat", "0xABC", "0xABC"));
    ASSERT_TRUE(w.push(1, kArgIn, "WAVEFORMATEX", "*pFormat", "48000/32f/2", "48000/32f/2"));
    ASSERT_TRUE(w.push(2, kArgIn, "WORD", "wFormatTag", "WAVE_FORMAT_EXTENSIBLE (0xFFFE)",
                       "WAVE_FORMAT_EXTENSIBLE (0xFFFE)"));
    ASSERT_TRUE(w.push(2, kArgIn, "WORD", "nChannels", "2", "2"));
    ASSERT_TRUE(w.push(0, kArgRet, "HRESULT", "Return", "S_OK", "S_OK"));
    ASSERT_GT(w.len, 2u);
    ASSERT_EQ(w.count, 6u);

    std::vector<CallArg> args;
    ASSERT_TRUE(decodeCallArgBlob(blob, w.len, args));
    ASSERT_EQ(args.size(), 6u);
    EXPECT_EQ(args[0].name, "ShareMode");
    EXPECT_EQ(args[0].pre, "AUDCLNT_SHAREMODE_SHARED");
    EXPECT_EQ(args[0].post, "AUDCLNT_SHAREMODE_SHARED");
    EXPECT_EQ(args[1].type, "WAVEFORMATEX*");
    EXPECT_EQ(args[2].depth, 1);
    EXPECT_EQ(args[3].depth, 2);
    EXPECT_EQ(args[3].name, "wFormatTag");
    EXPECT_NE(args[3].pre.find("WAVE_FORMAT_EXTENSIBLE"), std::string::npos);
    EXPECT_EQ(args[5].name, "Return");
    EXPECT_EQ(args[5].dir, kArgRet);
    EXPECT_STREQ(callArgDirName(args[5].dir), "return");
}

TEST(CallArgBlob, RejectsEmptyOrBadVersion) {
    std::vector<CallArg> args;
    EXPECT_FALSE(decodeCallArgBlob(nullptr, 0, args));
    uint8_t bad[4] = {99, 0, 0, 0};
    EXPECT_FALSE(decodeCallArgBlob(bad, 4, args));
    EXPECT_TRUE(args.empty());
}

TEST(CallArgBlob, IncompleteSecondParameterDoesNotExposePartialTree) {
    uint8_t blob[kArgBlobCap] = {};
    ArgBlobWriter writer;
    writer.reset(blob, kArgBlobCap);
    ASSERT_TRUE(writer.push(0, kArgIn, "DWORD", "flags", "1", "1"));
    ASSERT_TRUE(writer.push(1, kArgOut, "UINT", "frames", "0", "480"));
    std::vector<CallArg> args;
    EXPECT_FALSE(decodeCallArgBlob(blob, static_cast<uint16_t>(writer.len - 1), args));
    EXPECT_TRUE(args.empty());
}
