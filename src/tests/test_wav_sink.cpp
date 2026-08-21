#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <gtest/gtest.h>
#include <windows.h>
#include <cstdint>
#include <regex>
#include <string>
#include <vector>
#include "AudioFormat.h"
#include "WavFile.h"
#include "WavSink.h"

using wa::AudioFormat;
using wa::WavReader;
using wa::WavSink;
using wa::WavSinkState;

namespace {

std::wstring tempDir() {
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    return dir;
}

std::wstring tempWav(const wchar_t* tag) {
    return tempDir() + L"wa_sink_" + tag + L"_" + std::to_wstring(GetTickCount64()) + L".wav";
}

std::string narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

} // namespace

TEST(WavSink, ExactPathRoundTrip16BitStereo) {
    AudioFormat fmt{48000, 2, 16, false};
    std::vector<int16_t> samples(480 * 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(i - 100);
    const size_t bytes = samples.size() * sizeof(int16_t);
    const std::wstring path = tempWav(L"rt16");

    WavSink sink;
    ASSERT_TRUE(sink.startExact(path, fmt)) << sink.poll().message;
    EXPECT_EQ(sink.poll().state, WavSinkState::Running);
    EXPECT_EQ(sink.poll().path, path);
    ASSERT_EQ(sink.push(samples.data(), bytes), bytes);
    ASSERT_TRUE(sink.stop());
    EXPECT_EQ(sink.poll().state, WavSinkState::Idle);

    WavReader r;
    const wa::Result opened = r.open(path);
    ASSERT_TRUE(opened) << opened.message;
    EXPECT_TRUE(r.format() == fmt);
    std::vector<int16_t> back(samples.size());
    EXPECT_EQ(r.read(back.data(), bytes), bytes);
    EXPECT_EQ(samples, back);
    r.close();
    DeleteFileW(path.c_str());
}

TEST(WavSink, ExactPathRoundTripFloat32Stereo) {
    AudioFormat fmt{48000, 2, 32, true};
    std::vector<float> samples(240 * 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(i) * 0.001f - 0.1f;
    const size_t bytes = samples.size() * sizeof(float);
    const std::wstring path = tempWav(L"rt32f");

    WavSink sink;
    ASSERT_TRUE(sink.startExact(path, fmt)) << sink.poll().message;
    ASSERT_EQ(sink.push(samples.data(), bytes), bytes);
    ASSERT_TRUE(sink.stop());

    WavReader r;
    ASSERT_TRUE(r.open(path));
    EXPECT_TRUE(r.format() == fmt);
    std::vector<float> back(samples.size());
    EXPECT_EQ(r.read(back.data(), bytes), bytes);
    EXPECT_EQ(samples, back);
    r.close();
    DeleteFileW(path.c_str());
}

TEST(WavSink, ExactPathKeepsCallerName) {
    AudioFormat fmt{48000, 2, 16, false};
    const std::wstring path = tempWav(L"keepname");
    WavSink sink;
    ASSERT_TRUE(sink.startExact(path, fmt));
    EXPECT_EQ(sink.poll().path, path);
    EXPECT_EQ(sink.poll().fileName, path.substr(path.find_last_of(L"\\/") + 1));
    sink.stop();
    DeleteFileW(path.c_str());
}

TEST(WavSink, InvalidPathFailsWithoutThrowing) {
    AudioFormat fmt{48000, 2, 16, false};
    WavSink sink;
    EXPECT_FALSE(sink.startExact(L"?:\\wa_sink_not_a_path.wav", fmt));
    EXPECT_EQ(sink.poll().state, WavSinkState::Error);
}

TEST(WavSink, AutoNameUsesPrefixFormatAndTimestamp) {
    AudioFormat fmt{48000, 2, 16, false};
    const int16_t sample[2] = {1, -1};
    WavSink sink;
    ASSERT_TRUE(sink.start(tempDir(), "loopback", fmt)) << sink.poll().message;
    const std::wstring name = sink.poll().fileName;
    ASSERT_EQ(sink.push(sample, sizeof(sample)), sizeof(sample));
    ASSERT_TRUE(sink.stop());

    const std::string nameA = narrow(name);
    EXPECT_TRUE(std::regex_match(nameA, std::regex(
        R"(loopback_48000_2ch_16_\d{8}_\d{6}\.wav)"))) << nameA;
    DeleteFileW(sink.poll().path.c_str());
}

TEST(WavSink, AutoNameCollisionGetsNumericSuffix) {
    AudioFormat fmt{44100, 1, 24, false};
    WavSink first;
    ASSERT_TRUE(first.start(tempDir(), "loopback", fmt)) << first.poll().message;
    const std::wstring firstPath = first.poll().path;
    const std::wstring firstName = first.poll().fileName;
    ASSERT_TRUE(first.stop());

    WavSink second;
    ASSERT_TRUE(second.start(tempDir(), "loopback", fmt)) << second.poll().message;
    const std::wstring secondName = second.poll().fileName;
    second.stop();

    EXPECT_NE(secondName, firstName);
    const std::string a = narrow(firstName);
    const std::string b = narrow(secondName);
    const std::regex stem(R"(loopback_44100_1ch_24_\d{8}_\d{6}(_\d+)?\.wav)");
    EXPECT_TRUE(std::regex_match(a, stem)) << a;
    EXPECT_TRUE(std::regex_match(b, stem)) << b;
    DeleteFileW(firstPath.c_str());
    DeleteFileW(second.poll().path.c_str());
}

TEST(WavSink, OverflowFailsSinkAndPatchesHeader) {
    AudioFormat fmt{48000, 2, 16, false};
    const std::wstring path = tempWav(L"ovf");
    WavSink sink;
    ASSERT_TRUE(sink.startExact(path, fmt));
    std::vector<uint8_t> blob(2u * 1024u * 1024u + 64u, 0x11);
    const size_t n = sink.push(blob.data(), blob.size());
    EXPECT_LT(n, blob.size());
    EXPECT_FALSE(sink.stop());
    EXPECT_EQ(sink.poll().state, WavSinkState::Error);

    WavReader r;
    ASSERT_TRUE(r.open(path));
    EXPECT_TRUE(r.format() == fmt);
    EXPECT_GT(r.format().sampleRate, 0u);
    r.close();
    DeleteFileW(path.c_str());
}

TEST(WavSink, TwoSinksDoNotMixBytes) {
    AudioFormat fmt{48000, 2, 16, false};
    std::vector<int16_t> a(64, 1111);
    std::vector<int16_t> b(64, 2222);
    const std::wstring pa = tempWav(L"mixA");
    const std::wstring pb = tempWav(L"mixB");
    WavSink sa, sb;
    ASSERT_TRUE(sa.startExact(pa, fmt));
    ASSERT_TRUE(sb.startExact(pb, fmt));
    ASSERT_EQ(sa.push(a.data(), a.size() * 2), a.size() * 2);
    ASSERT_EQ(sb.push(b.data(), b.size() * 2), b.size() * 2);
    ASSERT_TRUE(sa.stop());
    ASSERT_TRUE(sb.stop());

    WavReader ra, rb;
    ASSERT_TRUE(ra.open(pa));
    ASSERT_TRUE(rb.open(pb));
    std::vector<int16_t> ba(64), bb(64);
    EXPECT_EQ(ra.read(ba.data(), ba.size() * 2), ba.size() * 2);
    EXPECT_EQ(rb.read(bb.data(), bb.size() * 2), bb.size() * 2);
    EXPECT_EQ(a, ba);
    EXPECT_EQ(b, bb);
    ra.close();
    rb.close();
    DeleteFileW(pa.c_str());
    DeleteFileW(pb.c_str());
}
