#include <gtest/gtest.h>

#include "Log.h"
#include "AudioFormatStr.h"

#include <windows.h>
#include <audioclient.h>

#include <mutex>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace wa;

TEST(AudioFormatStr, FormatAudio) {
    EXPECT_EQ(formatAudio({48000, 2, 16, false}), "48000/16/2");
    EXPECT_EQ(formatAudio({44100, 1, 24, false}), "44100/24/1");
    EXPECT_EQ(formatAudio({48000, 2, 32, true}), "48000/32f/2");
    EXPECT_EQ(formatAudio({96000, 1, 32, true}), "96000/32f/1");
}

TEST(AudioFormatStr, NarrowAscii) {
    EXPECT_EQ(narrowAscii(L""), "");
    EXPECT_EQ(narrowAscii(L"{0.0.1}.{abc-123}"), "{0.0.1}.{abc-123}");
}

TEST(Log, HrNameKnownSymbols) {
    EXPECT_EQ(log::hrName(S_OK), "S_OK");
    EXPECT_EQ(log::hrName(S_FALSE), "S_FALSE");
    EXPECT_EQ(log::hrName(AUDCLNT_E_DEVICE_INVALIDATED), "AUDCLNT_E_DEVICE_INVALIDATED");
    EXPECT_EQ(log::hrName(AUDCLNT_E_UNSUPPORTED_FORMAT), "AUDCLNT_E_UNSUPPORTED_FORMAT");
}

TEST(Log, HrNameUnknownFallsBackToHex) {
    // Unknown HRESULT: never empty, contains the hex form.
    std::string s = log::hrName(0x12345678L);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("0x12345678"), std::string::npos);
}

TEST(Log, LevelFilterAndSinkDelivery) {
    std::mutex m;
    std::vector<log::Level> got;
    log::init();
    log::addCallbackSink([&](log::Level lv, const std::string&) {
        std::lock_guard<std::mutex> lk(m);
        got.push_back(lv);
    });
    log::setLevel(log::Level::Info);
    log::emit(log::Level::Debug, "T", "dbg", "", "");  // below Info -> filtered
    log::emit(log::Level::Info, "T", "inf", "", "");    // delivered
    log::emit(log::Level::Err, "T", "err", "", "");     // delivered
    log::shutdown();  // drains the async queue

    std::lock_guard<std::mutex> lk(m);
    EXPECT_EQ(got.size(), 2u);
    for (log::Level lv : got) EXPECT_NE(lv, log::Level::Debug);
}

TEST(Log, FormatsLevelAsShortMarker) {
    std::mutex m;
    std::vector<std::string> lines;
    log::init();
    log::setThreadName("test");
    log::addCallbackSink([&](log::Level, const std::string& line) {
        std::lock_guard<std::mutex> lk(m);
        lines.push_back(line);
    });
    log::setLevel(log::Level::Info);
    log::emit(log::Level::Info, "T", "inf", "", "");
    log::emit(log::Level::Warn, "T", "warn", "", "");
    log::emit(log::Level::Err, "T", "err", "", "");
    log::shutdown();

    std::lock_guard<std::mutex> lk(m);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_NE(lines[0].find("[I]"), std::string::npos);
    EXPECT_NE(lines[1].find("[W]"), std::string::npos);
    EXPECT_NE(lines[2].find("[E]"), std::string::npos);
    for (const auto& line : lines) {
        EXPECT_EQ(line.find("INFO"), std::string::npos);
        EXPECT_EQ(line.find("WARN"), std::string::npos);
        EXPECT_EQ(line.find("ERROR"), std::string::npos);
    }
}

namespace {

class LogFile : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
            ("wa_log_" + std::to_string(GetCurrentProcessId())) / info->name();
        std::filesystem::create_directories(dir_);
        log::init();
    }

    void TearDown() override {
        log::shutdown();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::remove(dir_.parent_path(), ec);
    }

    std::filesystem::path dir_;
};

} // namespace

TEST_F(LogFile, OpenFailurePreservesPanelDelivery) {
    const auto path = dir_ / "winaudio.log";
    ASSERT_TRUE(std::filesystem::create_directory(path));
    std::mutex m;
    std::vector<std::string> lines;
    log::addCallbackSink([&](log::Level, const std::string& line) {
        std::lock_guard<std::mutex> lk(m);
        lines.push_back(line);
    });

    Result result;
    EXPECT_NO_THROW(result = log::addFileSink(path.string()));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.code, E_FAIL);
    EXPECT_NE(result.message.find("winaudio.log"), std::string::npos);
    log::emit(log::Level::Warn, "Test", "startup", "", "panel remains available");
    log::shutdown();

    std::lock_guard<std::mutex> lk(m);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("panel remains available"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_directory(path));
}

TEST_F(LogFile, WritesToRequestedFile) {
    const auto path = dir_ / "winaudio.log";
    Result result;
    EXPECT_NO_THROW(result = log::addFileSink(path.string()));
    EXPECT_TRUE(result) << result.message;
    log::emit(log::Level::Info, "Test", "startup", "", "file remains available");
    log::shutdown();

    std::ifstream file(path);
    ASSERT_TRUE(file.good());
    const std::string text{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("file remains available"), std::string::npos);
}
