#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <windows.h>
#include <cstdio>
#include "WavFile.h"
#include "AudioFormat.h"

using wa::WavWriter;
using wa::WavReader;
using wa::AudioFormat;

static std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

TEST(WavFile, RoundTrip16BitStereo) {
    AudioFormat fmt{48000, 2, 16, false};
    std::vector<int16_t> samples(480 * 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(i - 100);

    std::wstring path = TempPath(L"wa_roundtrip.wav");
    {
        WavWriter w;
        ASSERT_TRUE(w.open(path, fmt));
        ASSERT_EQ(w.write(samples.data(), samples.size() * 2),
                  samples.size() * 2);
        ASSERT_TRUE(w.close());
    }
    {
        WavReader r;
        ASSERT_TRUE(r.open(path));
        EXPECT_TRUE(r.format() == fmt);
        std::vector<int16_t> back(samples.size());
        EXPECT_EQ(r.read(back.data(), back.size() * 2), back.size() * 2);
        EXPECT_EQ(samples, back);
        uint8_t extra;
        EXPECT_EQ(r.read(&extra, 1), 0u);
        EXPECT_TRUE(r.eof());
    }
    _wremove(path.c_str());
}

TEST(WavFile, OpenMissingFileFails) {
    WavReader r;
    EXPECT_FALSE(r.open(L"Z:\\does\\not\\exist_xyz.wav"));
}

TEST(WavFile, RejectsTruncatedHeader) {
    std::wstring path = TempPath(L"wa_trunc.wav");
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    ASSERT_NE(f, nullptr);
    const char junk[8] = {'R','I','F','F', 1, 0, 0, 0};
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    WavReader r;
    EXPECT_FALSE(r.open(path));
    _wremove(path.c_str());
}
