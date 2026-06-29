#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include "SampleConvert.h"
using namespace wa;

TEST(SampleConvert, Int16BoundsRoundTrip) {
    AudioFormat f{48000,1,16,false};
    int16_t in[3] = {INT16_MIN, 0, INT16_MAX};
    float fl[3]; pcmToFloat(reinterpret_cast<uint8_t*>(in), 3, f, fl);
    EXPECT_NEAR(fl[0], -1.0f, 1e-6f);             // INT16_MIN/32768 = -1.0
    EXPECT_NEAR(fl[1], 0.0f, 1e-6f);
    EXPECT_NEAR(fl[2], 32767.0f/32768.0f, 1e-6f);
    int16_t back[3]; floatToPcm(fl, 3, f, reinterpret_cast<uint8_t*>(back));
    EXPECT_EQ(back[0], INT16_MIN); EXPECT_EQ(back[2], INT16_MAX);
}
TEST(SampleConvert, FloatToPcmClampsOverflow) {
    AudioFormat f{48000,1,16,false};
    float over[2] = {2.0f, -2.0f};                 // out of range -> must clamp, no UB
    int16_t out[2]; floatToPcm(over, 2, f, reinterpret_cast<uint8_t*>(out));
    EXPECT_EQ(out[0], INT16_MAX); EXPECT_EQ(out[1], INT16_MIN);
}
TEST(SampleConvert, Int24NegativeSignExtend) {
    AudioFormat f{48000,1,24,false};
    uint8_t in[3] = {0x00,0x00,0x80};              // little-endian 0x800000 = most-negative 24-bit
    float fl; pcmToFloat(in, 1, f, &fl);
    EXPECT_LT(fl, -0.99f);                          // must be ~ -1.0, not a large positive
}
TEST(SampleConvert, DownmixAverages) {
    float st[4] = {1.0f,1.0f, 1.0f,-1.0f};         // frame0 L=R=1 -> 1.0 ; frame1 L=-R -> 0
    float mono[2]; downmixMono(st, 2, 2, mono);
    EXPECT_NEAR(mono[0], 1.0f, 1e-6f);
    EXPECT_NEAR(mono[1], 0.0f, 1e-6f);              // antiphase cancels (documented limitation)
}
TEST(SampleConvert, PeakLevel) {
    float x[4] = {0.1f,-0.5f,0.3f,-0.2f};
    EXPECT_NEAR(peakLevel(x,4), 0.5f, 1e-6f);
}
