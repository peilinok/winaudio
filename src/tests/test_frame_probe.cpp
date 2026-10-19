#include <gtest/gtest.h>

#include "FrameProbe.h"

#include <cstring>
#include <string>

namespace {

void burn() {
    volatile uint32_t acc = 0;
    for (uint32_t i = 0; i < 200000u; ++i)
        acc += i;
}

}  // namespace

TEST(FrameProbe, DisabledLeavesSlotEmpty) {
    wa::FrameProbe probe;
    EXPECT_FALSE(probe.enabled());
    probe.beginFrame();
    probe.begin(wa::FrameSlice::LogDrain);
    burn();
    probe.end(wa::FrameSlice::LogDrain);
    EXPECT_EQ(probe.slot(wa::FrameSlice::LogDrain).n, 0u);
    EXPECT_EQ(probe.slot(wa::FrameSlice::LogDrain).ticks, 0u);

    char buf[32];
    buf[0] = 'Q';
    EXPECT_EQ(probe.formatLine(wa::FrameSlice::LogDrain, buf, static_cast<int>(sizeof(buf))), 0);
    EXPECT_EQ(buf[0], 'Q');
}

TEST(FrameProbe, EndWithoutBeginDoesNotCount) {
    wa::FrameProbe probe;
    probe.setEnabled(true);
    probe.end(wa::FrameSlice::Present);
    EXPECT_EQ(probe.slot(wa::FrameSlice::Present).n, 0u);
}

TEST(FrameProbe, OneBracketRecordsTicks) {
    wa::FrameProbe probe;
    probe.setEnabled(true);
    ASSERT_GT(probe.qpcFrequency(), 0u);
    probe.beginFrame();
    probe.begin(wa::FrameSlice::Heatmap);
    burn();
    probe.end(wa::FrameSlice::Heatmap);

    const wa::FrameProbe::Slot& slot = probe.slot(wa::FrameSlice::Heatmap);
    EXPECT_EQ(slot.n, 1u);
    EXPECT_GT(slot.ticks, 1u);
    EXPECT_EQ(probe.slot(wa::FrameSlice::Envelope).n, 0u);
}

TEST(FrameProbe, TwoBracketsAccumulateOneSlot) {
    wa::FrameProbe probe;
    probe.setEnabled(true);
    probe.beginFrame();
    probe.begin(wa::FrameSlice::Envelope);
    burn();
    probe.end(wa::FrameSlice::Envelope);
    const uint64_t first = probe.slot(wa::FrameSlice::Envelope).ticks;
    ASSERT_GT(first, 1u);

    probe.begin(wa::FrameSlice::Envelope);
    burn();
    probe.end(wa::FrameSlice::Envelope);
    const wa::FrameProbe::Slot& slot = probe.slot(wa::FrameSlice::Envelope);
    EXPECT_EQ(slot.n, 2u);
    EXPECT_GT(slot.ticks, first);
}

TEST(FrameProbe, BeginFrameClearsSlots) {
    wa::FrameProbe probe;
    probe.setEnabled(true);
    probe.begin(wa::FrameSlice::Present);
    burn();
    probe.end(wa::FrameSlice::Present);
    ASSERT_EQ(probe.slot(wa::FrameSlice::Present).n, 1u);
    probe.beginFrame();
    EXPECT_EQ(probe.slot(wa::FrameSlice::Present).n, 0u);
    EXPECT_EQ(probe.slot(wa::FrameSlice::Present).ticks, 0u);
}

TEST(FrameProbe, FormatLineUsesTheCiteableFields) {
    wa::FrameProbe probe;
    probe.setEnabled(true);
    probe.begin(wa::FrameSlice::LogDrain);
    burn();
    probe.end(wa::FrameSlice::LogDrain);
    const wa::FrameProbe::Slot& slot = probe.slot(wa::FrameSlice::LogDrain);
    ASSERT_EQ(slot.n, 1u);
    ASSERT_GT(slot.ticks, 1u);

    const uint64_t freq = probe.qpcFrequency();
    const uint64_t whole = slot.ticks / freq;
    const uint64_t rem = slot.ticks % freq;
    const uint64_t nanos = whole * 1000000000ull + (rem * 1000000000ull) / freq;

    char buf[640];
    const int n = probe.formatLine(wa::FrameSlice::LogDrain, buf, static_cast<int>(sizeof(buf)));
    ASSERT_GT(n, 0);
    EXPECT_EQ(buf[n], '\0');
    const std::string line(buf, static_cast<size_t>(n));

    EXPECT_NE(line.find("slice=log-drain "), std::string::npos);
#ifdef NDEBUG
    EXPECT_NE(line.find("build=Release "), std::string::npos);
    EXPECT_NE(line.find("\\Release\\WinAudioGui.exe\n"), std::string::npos);
#else
    EXPECT_NE(line.find("build=Debug "), std::string::npos);
    EXPECT_NE(line.find("\\Debug\\WinAudioGui.exe\n"), std::string::npos);
#endif
    EXPECT_NE(line.find("N=1 "), std::string::npos);
    EXPECT_NE(line.find("ticks=" + std::to_string(slot.ticks) + " "), std::string::npos);
    EXPECT_NE(line.find("qpc_freq=" + std::to_string(freq) + " "), std::string::npos);
    EXPECT_NE(line.find("nanos=" + std::to_string(nanos) + " "), std::string::npos);
    EXPECT_NE(line.find("uncertainty_ticks=1 "), std::string::npos);
    EXPECT_NE(line.find(std::string("scene=") + wa::FrameProbe::sceneLabel() + " "),
              std::string::npos);
    EXPECT_NE(line.find("command=set WINAUDIO_FRAME_PROBE=1 & "), std::string::npos);
    EXPECT_EQ(line.find("CPU="), std::string::npos);
    EXPECT_EQ(line.find("GPU="), std::string::npos);
    EXPECT_EQ(line.find("memory="), std::string::npos);
    EXPECT_EQ(line.find("refresh_hz="), std::string::npos);
    EXPECT_EQ(line.find("power_mode="), std::string::npos);
    EXPECT_EQ(line.find("windows="), std::string::npos);
}

TEST(FrameProbe, TwoHitsReportUncertaintyOfTwo) {
    wa::FrameProbe probe;
    probe.setEnabled(true);
    probe.begin(wa::FrameSlice::Heatmap);
    burn();
    probe.end(wa::FrameSlice::Heatmap);
    probe.begin(wa::FrameSlice::Heatmap);
    burn();
    probe.end(wa::FrameSlice::Heatmap);

    char buf[640];
    ASSERT_GT(probe.formatLine(wa::FrameSlice::Heatmap, buf, static_cast<int>(sizeof(buf))), 0);
    const std::string line(buf);
    EXPECT_NE(line.find("slice=heatmap-submit "), std::string::npos);
    EXPECT_NE(line.find("N=2 "), std::string::npos);
    EXPECT_NE(line.find("uncertainty_ticks=2 "), std::string::npos);
}

TEST(FrameProbe, EmptySliceDoesNotFormat) {
    wa::FrameProbe probe;
    probe.setEnabled(true);
    probe.begin(wa::FrameSlice::Heatmap);
    burn();
    probe.end(wa::FrameSlice::Heatmap);

    char buf[32];
    std::memset(buf, 'Z', sizeof(buf));
    EXPECT_EQ(probe.formatLine(wa::FrameSlice::Present, buf, static_cast<int>(sizeof(buf))), 0);
    EXPECT_EQ(buf[0], 'Z');
}

TEST(FrameProbe, HeaderNamesFactsButDoesNotFillThem) {
    const char* header = wa::FrameProbe::headerLine();
    ASSERT_NE(header, nullptr);
    EXPECT_NE(std::strstr(header, "not detected"), nullptr);
    EXPECT_NE(std::strstr(header, "does not detect"), nullptr);
    EXPECT_NE(std::strstr(header, "CPU"), nullptr);
    EXPECT_NE(std::strstr(header, "GPU"), nullptr);
    EXPECT_NE(std::strstr(header, "memory"), nullptr);
    EXPECT_NE(std::strstr(header, "refresh_hz"), nullptr);
    EXPECT_NE(std::strstr(header, "power_mode"), nullptr);
    EXPECT_NE(std::strstr(header, "windows"), nullptr);
    EXPECT_EQ(std::strstr(header, "CPU="), nullptr);
    EXPECT_EQ(std::strstr(header, "GPU="), nullptr);
    EXPECT_EQ(std::strstr(header, "memory="), nullptr);
    EXPECT_EQ(std::strstr(header, "refresh_hz="), nullptr);
    EXPECT_EQ(std::strstr(header, "power_mode="), nullptr);
    EXPECT_EQ(std::strstr(header, "windows="), nullptr);
}

TEST(FrameProbe, SliceNamesMatchTheFourRecords) {
    EXPECT_STREQ(wa::FrameProbe::sliceName(wa::FrameSlice::Heatmap), "heatmap-submit");
    EXPECT_STREQ(wa::FrameProbe::sliceName(wa::FrameSlice::Envelope), "waveform-envelope");
    EXPECT_STREQ(wa::FrameProbe::sliceName(wa::FrameSlice::LogDrain), "log-drain");
    EXPECT_STREQ(wa::FrameProbe::sliceName(wa::FrameSlice::Present), "present");
}
