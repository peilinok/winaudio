#include <gtest/gtest.h>
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>
#include "AudioFormat.h"

using wa::AudioFormat;
using wa::toWaveFormatExtensible;
using wa::fromWaveFormat;

TEST(AudioFormat, DerivedFields) {
    AudioFormat f{48000, 2, 16, false};
    EXPECT_EQ(f.blockAlign(), 4u);          // 2ch * 2 bytes
    EXPECT_EQ(f.avgBytesPerSec(), 192000u); // 48000 * 4
}

TEST(AudioFormat, RoundTripPcm16) {
    AudioFormat f{44100, 2, 16, false};
    WAVEFORMATEXTENSIBLE w = toWaveFormatExtensible(f);
    AudioFormat back = fromWaveFormat(
        reinterpret_cast<const WAVEFORMATEX*>(&w));
    EXPECT_TRUE(f == back);
}

TEST(AudioFormat, RoundTripFloat32) {
    AudioFormat f{48000, 2, 32, true};
    WAVEFORMATEXTENSIBLE w = toWaveFormatExtensible(f);
    EXPECT_EQ(w.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    AudioFormat back = fromWaveFormat(
        reinterpret_cast<const WAVEFORMATEX*>(&w));
    EXPECT_TRUE(f == back);
}

TEST(AudioFormat, ParsePlainPcmWaveFormatEx) {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = 16000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = 32000;
    AudioFormat f = fromWaveFormat(&wf);
    EXPECT_EQ(f.sampleRate, 16000u);
    EXPECT_EQ(f.channels, 1);
    EXPECT_EQ(f.bitsPerSample, 16);
    EXPECT_FALSE(f.isFloat);
}

TEST(AudioFormat, ChannelMaskForCommonLayouts) {
    AudioFormat af1{48000, 1, 16, false};
    auto w1 = toWaveFormatExtensible(af1);
    EXPECT_EQ(w1.dwChannelMask, static_cast<DWORD>(SPEAKER_FRONT_CENTER));

    AudioFormat af2{48000, 2, 16, false};
    auto w2 = toWaveFormatExtensible(af2);
    auto mask2 = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    EXPECT_EQ(w2.dwChannelMask, static_cast<DWORD>(mask2));

    AudioFormat af6{48000, 6, 16, false};
    auto w6 = toWaveFormatExtensible(af6);
    auto mask6 = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                 SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    EXPECT_EQ(w6.dwChannelMask, static_cast<DWORD>(mask6));

    AudioFormat af3{48000, 3, 16, false};
    auto w3 = toWaveFormatExtensible(af3);
    EXPECT_EQ(w3.dwChannelMask, 0u); // unusual count -> unspecified
}
