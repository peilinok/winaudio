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
        ASSERT_TRUE(w.write(samples.data(), samples.size() * 2));
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

TEST(WavFile, ReadsExtensibleFloatHeader) {
    std::wstring path = TempPath(L"wa_ext.wav");
    FILE* f = nullptr; _wfopen_s(&f, path.c_str(), L"wb");
    ASSERT_NE(f, nullptr);
    auto w32 = [&](uint32_t v){ fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v){ fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(4 + 8 + 40 + 8 + 8); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(40);
    w16(0xFFFE);          // WAVE_FORMAT_EXTENSIBLE
    w16(2);               // channels
    w32(48000);           // sampleRate
    w32(48000u * 2u * 4u);// avgBytesPerSec
    w16(8);               // blockAlign
    w16(32);              // bitsPerSample
    w16(22);              // cbSize
    w16(32);              // wValidBitsPerSample
    w32(0x3);             // dwChannelMask
    w32(3);               // SubFormat GUID Data1 = IEEE_FLOAT
    w16(0); w16(0x10);    // Data2, Data3
    fwrite("\x80\x00\x00\xaa\x00\x38\x9b\x71", 1, 8, f); // Data4
    fwrite("data", 1, 4, f); w32(8); w32(0); w32(0);     // 8-byte data chunk
    fclose(f);

    WavReader r;
    ASSERT_TRUE(r.open(path));
    EXPECT_EQ(r.format().sampleRate, 48000u);
    EXPECT_EQ(r.format().channels, 2);
    EXPECT_EQ(r.format().bitsPerSample, 32);
    EXPECT_TRUE(r.format().isFloat); // EXTENSIBLE float must be recognized
    r.close();
    _wremove(path.c_str());
}

TEST(WavFile, WritesExtensible7Point1FloatWithFactChunk) {
    AudioFormat fmt{44100, 8, 32, true};
    fmt.validBitsPerSample = 32;
    fmt.channelMask = 0x63F;
    const std::vector<float> samples = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f};
    const std::wstring path = TempPath(L"wa_extensible_7_1_float.wav");

    WavWriter writer;
    ASSERT_TRUE(writer.open(path, fmt));
    ASSERT_TRUE(writer.write(samples.data(), samples.size() * sizeof(float)));
    ASSERT_TRUE(writer.close());

    FILE* f = nullptr;
    ASSERT_EQ(_wfopen_s(&f, path.c_str(), L"rb"), 0);
    ASSERT_NE(f, nullptr);
    uint8_t header[80]{};
    ASSERT_EQ(fread(header, 1, sizeof(header), f), sizeof(header));
    fclose(f);
    auto u16 = [&](size_t offset) {
        uint16_t value = 0;
        memcpy(&value, header + offset, sizeof(value));
        return value;
    };
    auto u32 = [&](size_t offset) {
        uint32_t value = 0;
        memcpy(&value, header + offset, sizeof(value));
        return value;
    };
    EXPECT_EQ(std::string(reinterpret_cast<char*>(header + 12), 4), "fmt ");
    EXPECT_EQ(u32(16), 40u);
    EXPECT_EQ(u16(20), static_cast<uint16_t>(WAVE_FORMAT_EXTENSIBLE));
    EXPECT_EQ(u16(38), 32u);
    EXPECT_EQ(u32(40), 0x63Fu);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(header + 60), 4), "fact");
    EXPECT_EQ(u32(68), 1u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(header + 72), 4), "data");
    EXPECT_EQ(u32(76), 32u);

    WavReader reader;
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(reader.format().validBitsPerSample, 32);
    EXPECT_EQ(reader.format().channelMask, 0x63Fu);
    reader.close();
    _wremove(path.c_str());
}

TEST(WavFile, BuffersPartialFramesAcrossWrites) {
    AudioFormat fmt{48000, 6, 24, false};
    fmt.channelMask = 0x3F;
    std::vector<uint8_t> samples(36);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<uint8_t>(i + 1);
    const std::wstring path = TempPath(L"wa_partial_frame_chunks.wav");

    WavWriter writer;
    ASSERT_TRUE(writer.open(path, fmt));
    wa::Result first = writer.write(samples.data(), 5);
    ASSERT_TRUE(first);
    wa::Result second = writer.write(samples.data() + 5, samples.size() - 5);
    ASSERT_TRUE(second);
    ASSERT_TRUE(writer.close());

    WavReader reader;
    ASSERT_TRUE(reader.open(path));
    std::vector<uint8_t> back(samples.size());
    EXPECT_EQ(reader.read(back.data(), back.size()), back.size());
    EXPECT_EQ(back, samples);
    reader.close();
    _wremove(path.c_str());
}

TEST(WavFile, RejectsUnsupportedFormatAtOpen) {
    const std::wstring path = TempPath(L"wa_invalid_float16.wav");
    WavWriter writer;
    EXPECT_FALSE(writer.open(path, AudioFormat{48000, 2, 16, true}));
    _wremove(path.c_str());
}

TEST(WavFile, CloseRejectsIncompleteFinalFrame) {
    const std::wstring path = TempPath(L"wa_incomplete_frame.wav");
    WavWriter writer;
    ASSERT_TRUE(writer.open(path, AudioFormat{48000, 2, 16, false}));
    const uint8_t partial[3] = {1, 2, 3};
    ASSERT_TRUE(writer.write(partial, sizeof(partial)));
    const wa::Result closeResult = writer.close();
    EXPECT_FALSE(closeResult);
    EXPECT_NE(closeResult.message.find("incomplete audio frame"), std::string::npos);
    _wremove(path.c_str());
}

TEST(WavFile, ReaderRejectsUnknownExtensibleSubFormat) {
    const std::wstring path = TempPath(L"wa_unknown_subformat.wav");
    FILE* f = nullptr;
    ASSERT_EQ(_wfopen_s(&f, path.c_str(), L"wb"), 0);
    ASSERT_NE(f, nullptr);
    const uint32_t riffSize = 4 + 8 + 40 + 8;
    const uint32_t fmtSize = 40;
    const uint32_t dataSize = 0;
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = 2;
    w.Format.nSamplesPerSec = 48000;
    w.Format.nAvgBytesPerSec = 384000;
    w.Format.nBlockAlign = 8;
    w.Format.wBitsPerSample = 32;
    w.Format.cbSize = 22;
    w.Samples.wValidBitsPerSample = 32;
    w.dwChannelMask = 0x3;
    w.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    w.SubFormat.Data4[7] ^= 0x1;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtSize, 4, 1, f); fwrite(&w, 1, sizeof(w), f);
    fwrite("data", 1, 4, f); fwrite(&dataSize, 4, 1, f);
    fclose(f);

    WavReader reader;
    EXPECT_FALSE(reader.open(path));
    _wremove(path.c_str());
}

TEST(WavFile, ReaderRejectsExtensibleCbSizeBeyondFmtChunk) {
    const std::wstring path = TempPath(L"wa_invalid_extensible_cbsize.wav");
    FILE* f = nullptr;
    ASSERT_EQ(_wfopen_s(&f, path.c_str(), L"wb"), 0);
    ASSERT_NE(f, nullptr);
    const uint32_t riffSize = 4 + 8 + 40 + 8;
    const uint32_t fmtSize = 40;
    const uint32_t dataSize = 0;
    WAVEFORMATEXTENSIBLE w = wa::toWaveFormatExtensible(
        AudioFormat{48000, 2, 32, true});
    w.Format.cbSize = 23; // fmt chunk has room for only the required 22 extension bytes
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtSize, 4, 1, f); fwrite(&w, 1, sizeof(w), f);
    fwrite("data", 1, 4, f); fwrite(&dataSize, 4, 1, f);
    fclose(f);

    WavReader reader;
    EXPECT_FALSE(reader.open(path));
    _wremove(path.c_str());
}

TEST(WavFile, ReaderRejectsChannelMaskThatDoesNotMatchChannels) {
    const std::wstring path = TempPath(L"wa_invalid_channel_mask.wav");
    FILE* f = nullptr;
    ASSERT_EQ(_wfopen_s(&f, path.c_str(), L"wb"), 0);
    ASSERT_NE(f, nullptr);
    const uint32_t riffSize = 4 + 8 + 40 + 8;
    const uint32_t fmtSize = 40;
    const uint32_t dataSize = 0;
    WAVEFORMATEXTENSIBLE w = wa::toWaveFormatExtensible(
        AudioFormat{48000, 2, 32, true});
    w.dwChannelMask = SPEAKER_FRONT_CENTER;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtSize, 4, 1, f); fwrite(&w, 1, sizeof(w), f);
    fwrite("data", 1, 4, f); fwrite(&dataSize, 4, 1, f);
    fclose(f);

    WavReader reader;
    EXPECT_FALSE(reader.open(path));
    _wremove(path.c_str());
}

TEST(WavFile, ReaderSkipsOddSizedUnknownChunkWithPadding) {
    const std::wstring path = TempPath(L"wa_odd_unknown_chunk.wav");
    FILE* f = nullptr;
    ASSERT_EQ(_wfopen_s(&f, path.c_str(), L"wb"), 0);
    ASSERT_NE(f, nullptr);
    auto w32 = [&](uint32_t value) { fwrite(&value, sizeof(value), 1, f); };
    auto w16 = [&](uint16_t value) { fwrite(&value, sizeof(value), 1, f); };
    const uint32_t riffSize = 4 + (8 + 16) + (8 + 3 + 1) + (8 + 4);
    fwrite("RIFF", 1, 4, f); w32(riffSize); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16);
    w16(WAVE_FORMAT_PCM); w16(1); w32(48000); w32(96000); w16(2); w16(16);
    fwrite("JUNK", 1, 4, f); w32(3); fwrite("abc\0", 1, 4, f);
    const int16_t samples[2] = {123, -456};
    fwrite("data", 1, 4, f); w32(sizeof(samples)); fwrite(samples, 1, sizeof(samples), f);
    fclose(f);

    WavReader reader;
    ASSERT_TRUE(reader.open(path));
    int16_t back[2]{};
    EXPECT_EQ(reader.read(back, sizeof(back)), sizeof(back));
    EXPECT_EQ(back[0], samples[0]);
    EXPECT_EQ(back[1], samples[1]);
    reader.close();
    _wremove(path.c_str());
}

TEST(WavFile, RoundTripsPcm24ValidBitsIn32BitContainer) {
    AudioFormat fmt{48000, 2, 32, false};
    fmt.validBitsPerSample = 24;
    fmt.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    const int32_t samples[2] = {0x00123400, -0x00123400};
    const std::wstring path = TempPath(L"wa_pcm24_in_32.wav");

    WavWriter writer;
    ASSERT_TRUE(writer.open(path, fmt));
    ASSERT_TRUE(writer.write(samples, sizeof(samples)));
    ASSERT_TRUE(writer.close());

    WavReader reader;
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(reader.format().bitsPerSample, 32);
    EXPECT_EQ(reader.format().validBitsPerSample, 24);
    EXPECT_EQ(reader.format().channelMask, 0x3u);
    int32_t back[2]{};
    EXPECT_EQ(reader.read(back, sizeof(back)), sizeof(back));
    EXPECT_EQ(back[0], samples[0]);
    EXPECT_EQ(back[1], samples[1]);
    reader.close();
    _wremove(path.c_str());
}

TEST(WavFile, WritesOddSizedEightBitMonoDataWithPadByte) {
    const AudioFormat fmt{8000, 1, 8, false};
    const uint8_t samples[3] = {1, 2, 3};
    const std::wstring path = TempPath(L"wa_odd_data_padding.wav");

    WavWriter writer;
    ASSERT_TRUE(writer.open(path, fmt));
    ASSERT_TRUE(writer.write(samples, sizeof(samples)));
    ASSERT_TRUE(writer.close());

    FILE* f = nullptr;
    ASSERT_EQ(_wfopen_s(&f, path.c_str(), L"rb"), 0);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(_fseeki64(f, 0, SEEK_END), 0);
    EXPECT_EQ(_ftelli64(f), 48);
    ASSERT_EQ(_fseeki64(f, 4, SEEK_SET), 0);
    uint32_t riffSize = 0;
    ASSERT_EQ(fread(&riffSize, sizeof(riffSize), 1, f), 1u);
    EXPECT_EQ(riffSize, 40u);
    ASSERT_EQ(_fseeki64(f, 40, SEEK_SET), 0);
    uint32_t dataSize = 0;
    ASSERT_EQ(fread(&dataSize, sizeof(dataSize), 1, f), 1u);
    EXPECT_EQ(dataSize, 3u);
    fclose(f);

    WavReader reader;
    ASSERT_TRUE(reader.open(path));
    uint8_t back[3]{};
    EXPECT_EQ(reader.read(back, sizeof(back)), sizeof(back));
    EXPECT_EQ(std::vector<uint8_t>(back, back + 3),
              std::vector<uint8_t>(samples, samples + 3));
    reader.close();
    _wremove(path.c_str());
}

TEST(WavFile, WriterRejectsDestinationThatCannotPatchHeader) {
    WavWriter writer;
    const wa::Result result = writer.open(L"NUL", AudioFormat{48000, 2, 16, false});
    EXPECT_FALSE(result);
}

TEST(WavFile, ReaderReuseDoesNotKeepExtensibleMetadata) {
    const std::wstring extensiblePath = TempPath(L"wa_reader_reuse_extensible.wav");
    const std::wstring pcmPath = TempPath(L"wa_reader_reuse_pcm.wav");
    {
        WavWriter writer;
        ASSERT_TRUE(writer.open(extensiblePath, AudioFormat{48000, 8, 32, true}));
        const float frame[8]{};
        ASSERT_TRUE(writer.write(frame, sizeof(frame)));
        ASSERT_TRUE(writer.close());
    }
    {
        WavWriter writer;
        ASSERT_TRUE(writer.open(pcmPath, AudioFormat{48000, 2, 16, false}));
        const int16_t frame[2]{};
        ASSERT_TRUE(writer.write(frame, sizeof(frame)));
        ASSERT_TRUE(writer.close());
    }

    WavReader reader;
    ASSERT_TRUE(reader.open(extensiblePath));
    EXPECT_EQ(reader.format().channelMask, 0x63Fu);
    ASSERT_TRUE(reader.close());
    ASSERT_TRUE(reader.open(pcmPath));
    EXPECT_EQ(reader.format().validBitsPerSample, 0);
    EXPECT_EQ(reader.format().channelMask, 0u);
    ASSERT_TRUE(reader.close());

    _wremove(extensiblePath.c_str());
    _wremove(pcmPath.c_str());
}
