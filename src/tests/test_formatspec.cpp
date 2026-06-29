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
    // 480 frames @ 48kHz = 10 ms = 100000 in 100-ns units.
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

TEST(FormatSpec, RejectsSignTrailingAndOutOfRange) {
    AudioFormat f{};
    EXPECT_FALSE(parseFormatSpec("-48000/16/2", f));    // leading sign
    EXPECT_FALSE(parseFormatSpec("48000/16/70000", f)); // channels out of range
    EXPECT_FALSE(parseFormatSpec("48000/32/2ff", f));   // trailing garbage after f
    EXPECT_FALSE(parseFormatSpec("48000/16/2 ", f));    // trailing space
    EXPECT_FALSE(parseFormatSpec("48000/16/2x", f));    // trailing junk
}
