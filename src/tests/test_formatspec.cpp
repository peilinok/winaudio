#include <gtest/gtest.h>
#include "FormatSpec.h"
using namespace wa;

TEST(FormatSpec, ParsesPcm) {
    AudioFormat f{};
    ASSERT_TRUE(parseFormatSpec("48000/16/2", f));
    EXPECT_EQ(f.sampleRate, 48000u);
    EXPECT_EQ(f.bitsPerSample, 16);
    EXPECT_EQ(f.channels, 2);
    EXPECT_FALSE(f.isFloat);
}

TEST(FormatSpec, ParsesFloatSuffix) {
    AudioFormat f{};
    ASSERT_TRUE(parseFormatSpec("48000/32/2f", f));
    EXPECT_EQ(f.bitsPerSample, 32);
    EXPECT_TRUE(f.isFloat);
}

TEST(FormatSpec, RejectsMalformed) {
    AudioFormat f{};
    EXPECT_FALSE(parseFormatSpec("48000/16", f));
    EXPECT_FALSE(parseFormatSpec("abc/16/2", f));
    EXPECT_FALSE(parseFormatSpec("", f));
}

TEST(FormatSpec, AlignedDurationFormula) {
    // 10000.0 * 1000 / 48000 * 480 + 0.5 = 100000 (100ms in 100ns units = 1,000,000? )
    // For 48000 Hz and 480 frames (10ms): 10000*1000/48000*480 = 100000 (100ns units) = 10 ms. round.
    long long d = alignedBufferDuration100ns(48000, 480);
    EXPECT_EQ(d, 100000); // 10 ms in 100-ns units
}

TEST(FormatSpec, SelectFirstSupported) {
    std::vector<AudioFormat> c = {
        {48000,2,16,false}, {44100,2,16,false}, {48000,2,32,true}
    };
    int idx = selectSupportedFormat(c, [](const AudioFormat& f){ return f.sampleRate == 44100; });
    EXPECT_EQ(idx, 1);
    int none = selectSupportedFormat(c, [](const AudioFormat&){ return false; });
    EXPECT_EQ(none, -1);
}

TEST(FormatSpec, CaptureCandidatesNonEmpty) {
    auto c = defaultExclusiveCaptureCandidates();
    ASSERT_FALSE(c.empty());
    EXPECT_EQ(c.front().sampleRate, 48000u); // 48k/16/2 first
    EXPECT_EQ(c.front().bitsPerSample, 16);
}
