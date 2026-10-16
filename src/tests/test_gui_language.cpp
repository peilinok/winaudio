#include <gtest/gtest.h>
#include "AppUiText.h"
#include "LogRegionPrefs.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeCaseDir() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    fs::path dir = fs::temp_directory_path() / "wa_gui_language" / info->name();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& p, const std::string& body) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out << body;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

class GuiLanguageText : public ::testing::Test {
protected:
    void TearDown() override {
        wa::ui_text::setLanguage(wa::gui_language::Language::En);
    }
};

}  // namespace

TEST(GuiLanguage, UserInterfacePrimaryChineseIncludesTraditionalAndHongKong) {
    EXPECT_TRUE(wa::gui_language::uiLanguageIsChinese(0x0804));  // simplified
    EXPECT_TRUE(wa::gui_language::uiLanguageIsChinese(0x0404));  // traditional
    EXPECT_TRUE(wa::gui_language::uiLanguageIsChinese(0x0C04));  // Hong Kong
    EXPECT_FALSE(wa::gui_language::uiLanguageIsChinese(0x0409)); // English UI
    EXPECT_FALSE(wa::gui_language::uiLanguageIsChinese(0x0407)); // German UI
}

TEST(GuiLanguage, MissingFileFollowsInjectedDisplayLanguage) {
    const fs::path missing = makeCaseDir() / "no-such.ini";
    const auto loaded = wa::log_region_prefs::load(missing);
    EXPECT_EQ(loaded.kind, wa::log_region_prefs::LoadKind::Missing);
    EXPECT_FALSE(loaded.hasLanguage);
    const auto zh = wa::log_region_prefs::resolve(loaded, true);
    EXPECT_EQ(zh.language, wa::gui_language::Language::ZhCn);
    EXPECT_FALSE(zh.fromOverride);
    EXPECT_FALSE(zh.logCollapsed);
    const auto en = wa::log_region_prefs::resolve(loaded, false);
    EXPECT_EQ(en.language, wa::gui_language::Language::En);
    EXPECT_FALSE(en.fromOverride);
}

TEST(GuiLanguage, MissingKeyEmptyAndIllegalValuesFollowDisplayLanguage) {
    const auto missingKey = wa::log_region_prefs::parse("log_collapsed=0\n");
    EXPECT_FALSE(missingKey.hasLanguage);
    EXPECT_EQ(wa::log_region_prefs::resolve(missingKey, true).language,
              wa::gui_language::Language::ZhCn);
    EXPECT_FALSE(wa::log_region_prefs::resolve(missingKey, true).fromOverride);

    const auto empty = wa::log_region_prefs::parse("language=\n");
    EXPECT_FALSE(empty.hasLanguage);
    EXPECT_EQ(wa::log_region_prefs::resolve(empty, false).language,
              wa::gui_language::Language::En);

    const auto other = wa::log_region_prefs::parse("language=zh\n");
    EXPECT_FALSE(other.hasLanguage);
    EXPECT_EQ(wa::log_region_prefs::resolve(other, true).language,
              wa::gui_language::Language::ZhCn);
    EXPECT_FALSE(wa::log_region_prefs::resolve(other, true).fromOverride);
}

TEST(GuiLanguage, StoredEnAndZhCnWinOverDisplayLanguage) {
    const auto en = wa::log_region_prefs::parse("[gui]\nlanguage=en\n");
    const auto enResolved = wa::log_region_prefs::resolve(en, true);
    EXPECT_TRUE(en.hasLanguage);
    EXPECT_EQ(enResolved.language, wa::gui_language::Language::En);
    EXPECT_TRUE(enResolved.fromOverride);

    const auto zh = wa::log_region_prefs::parse("language= zh-CN \n");
    const auto zhResolved = wa::log_region_prefs::resolve(zh, false);
    EXPECT_TRUE(zh.hasLanguage);
    EXPECT_EQ(zhResolved.language, wa::gui_language::Language::ZhCn);
    EXPECT_TRUE(zhResolved.fromOverride);
}

TEST(GuiLanguage, LastLanguageValueWinsAndIllegalLastFollowsDisplay) {
    const auto last = wa::log_region_prefs::parse("language=en\nlanguage=zh-CN\n");
    EXPECT_EQ(last.language, wa::gui_language::Language::ZhCn);
    EXPECT_TRUE(last.hasLanguage);

    const auto illegal = wa::log_region_prefs::parse("language=zh-CN\nlanguage=nope\n");
    EXPECT_FALSE(illegal.hasLanguage);
    EXPECT_EQ(wa::log_region_prefs::resolve(illegal, false).language,
              wa::gui_language::Language::En);
    EXPECT_EQ(wa::log_region_prefs::resolve(illegal, true).language,
              wa::gui_language::Language::ZhCn);
}

TEST(GuiLanguage, BareLogCollapsedStillLoadsAndIllegalLanguageKeepsIt) {
    const auto bare = wa::log_region_prefs::parse("log_collapsed=1\n");
    EXPECT_TRUE(bare.collapsed);
    EXPECT_EQ(bare.kind, wa::log_region_prefs::LoadKind::Ok);
    EXPECT_FALSE(bare.hasLanguage);

    const auto illegal = wa::log_region_prefs::parse("log_collapsed=1\nlanguage=foo\n");
    EXPECT_TRUE(illegal.collapsed);
    EXPECT_EQ(illegal.kind, wa::log_region_prefs::LoadKind::Ok);
    EXPECT_FALSE(illegal.hasLanguage);

    const auto beforeHeader = wa::log_region_prefs::parse(
        "log_collapsed=1\n[other]\nlanguage=zh-CN\n");
    EXPECT_TRUE(beforeHeader.collapsed);
    EXPECT_FALSE(beforeHeader.hasLanguage);
}

TEST(GuiLanguage, GuiSectionLanguageDoesNotDropLogBit) {
    const auto both = wa::log_region_prefs::parse(
        "[gui]\nlog_collapsed=1\nlanguage=zh-CN\n");
    EXPECT_TRUE(both.collapsed);
    EXPECT_TRUE(both.hasLanguage);
    EXPECT_EQ(both.language, wa::gui_language::Language::ZhCn);
    const auto resolved = wa::log_region_prefs::resolve(both, false);
    EXPECT_TRUE(resolved.logCollapsed);
    EXPECT_EQ(resolved.language, wa::gui_language::Language::ZhCn);
    EXPECT_TRUE(resolved.fromOverride);
}

TEST(GuiLanguage, SavingLanguageKeepsLogCollapsed) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    ASSERT_TRUE(wa::log_region_prefs::save(file, true));
    ASSERT_TRUE(wa::log_region_prefs::saveLanguage(
        file, wa::gui_language::Language::ZhCn, true));
    const auto loaded = wa::log_region_prefs::load(file);
    EXPECT_TRUE(loaded.collapsed);
    EXPECT_TRUE(loaded.hasLanguage);
    EXPECT_EQ(loaded.language, wa::gui_language::Language::ZhCn);

    ASSERT_TRUE(wa::log_region_prefs::save(file, false));
    const auto afterLog = wa::log_region_prefs::load(file);
    EXPECT_FALSE(afterLog.collapsed);
    EXPECT_TRUE(afterLog.hasLanguage);
    EXPECT_EQ(afterLog.language, wa::gui_language::Language::ZhCn);
}

TEST(GuiLanguage, SavingLogBeforeAnyLanguageChoiceDoesNotCreateLanguage) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    ASSERT_TRUE(wa::log_region_prefs::save(file, true));
    const std::string body = readFile(file);
    EXPECT_EQ(body.find("language"), std::string::npos);
    EXPECT_NE(body.find("[gui]"), std::string::npos);
    EXPECT_NE(body.find("log_collapsed=1"), std::string::npos);
    const auto loaded = wa::log_region_prefs::load(file);
    EXPECT_TRUE(loaded.collapsed);
    EXPECT_FALSE(loaded.hasLanguage);
}

TEST(GuiLanguage, UnreadablePathReportsFailure) {
    const fs::path dir = makeCaseDir();
    EXPECT_FALSE(wa::log_region_prefs::saveLanguage(
        dir, wa::gui_language::Language::En, false));
    EXPECT_FALSE(wa::log_region_prefs::save(dir, true));
}

TEST_F(GuiLanguageText, EnglishLanguageReturnsTheEnglishPointer) {
    wa::ui_text::setLanguage(wa::gui_language::Language::En);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kApply), wa::ui_text::kApply);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kLoopbackCreate), wa::ui_text::kLoopbackCreate);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kApplicationLoopbackTab),
              wa::ui_text::kApplicationLoopbackTab);
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kMmsysCplFailed),
                 "mmsys.cpl: ShellExecute failed");
}

TEST_F(GuiLanguageText, ChineseReturnsFilledSlotAndFallsBackWhenMissing) {
    wa::ui_text::setLanguage(wa::gui_language::Language::ZhCn);
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kApply), "应用");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kLoopbackCreate), "创建 Track");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kApplicationLoopbackTab), "Application Loopback");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kLoopbackEmptyHint), "创建 Track 以采集系统音频。");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kChartsPause), "暂停图表");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kLogEmpty), "（空）");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kMmsysCplFailed), "mmsys.cpl：ShellExecute 失败");
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kApply), wa::ui_text::get(wa::ui_text::kApply));

    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kLogExpand), wa::ui_text::kLogExpand);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kLogCollapse), wa::ui_text::kLogCollapse);
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kLogExpand), "+");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kLogCollapse), "-");
}

TEST_F(GuiLanguageText, LookupNeverReturnsNullAndPassesUnknownPointersThrough) {
    wa::ui_text::setLanguage(wa::gui_language::Language::ZhCn);
    EXPECT_NE(wa::ui_text::get(wa::ui_text::kApply), nullptr);
    EXPECT_NE(wa::ui_text::get(wa::ui_text::kLogExpand), nullptr);
    EXPECT_NE(wa::ui_text::get(nullptr), nullptr);
    EXPECT_STREQ(wa::ui_text::get(nullptr), "");
    const char extra[] = "not-a-catalog-sentence";
    EXPECT_EQ(wa::ui_text::get(extra), extra);
}

TEST_F(GuiLanguageText, SharedLabelsExistForLaterPages) {
    EXPECT_STREQ(wa::ui_text::kRefreshDevices, "Refresh devices");
    EXPECT_STREQ(wa::ui_text::kClose, "Close");
    EXPECT_STREQ(wa::ui_text::kFormat, "Format");
    EXPECT_STREQ(wa::ui_text::kIdle, "Idle");
    EXPECT_STREQ(wa::ui_text::kRunning, "Running");
    EXPECT_STREQ(wa::ui_text::kError, "Error");
    EXPECT_STREQ(wa::ui_text::kControl, "Control");
    EXPECT_STREQ(wa::ui_text::kSource, "Source");
    EXPECT_STREQ(wa::ui_text::kDevices, "Devices");
    EXPECT_STREQ(wa::ui_text::kStatus, "Status");
    EXPECT_STREQ(wa::ui_text::kStart, "Start");
    EXPECT_STREQ(wa::ui_text::kStop, "Stop");
    EXPECT_STREQ(wa::ui_text::kBackend, "Backend");
    EXPECT_STREQ(wa::ui_text::kDelay, "Delay");
    EXPECT_STREQ(wa::ui_text::kCategory, "Category");
    EXPECT_STREQ(wa::ui_text::kResetToSystemDefaults, "Reset to system defaults");
    EXPECT_STREQ(wa::ui_text::kDeviceCapabilities, "Device capabilities");
    EXPECT_STREQ(wa::ui_text::kCaptureCapabilities, "Capture capabilities");

    wa::ui_text::setLanguage(wa::gui_language::Language::En);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kRefreshDevices), wa::ui_text::kRefreshDevices);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kCaptureCapabilities),
              wa::ui_text::kCaptureCapabilities);
}

TEST_F(GuiLanguageText, HelpersThatReturnCatalogConstantsFollowLanguage) {
    wa::ui_text::setLanguage(wa::gui_language::Language::En);
    EXPECT_STREQ(wa::ui_text::pipelineCallLogEmptyText(false, false),
                 wa::ui_text::kPipelineCallLogEmpty);
    EXPECT_STREQ(wa::ui_text::channelPhraseText(wa::ChannelPhrase::None), "No phrase");

    wa::ui_text::setLanguage(wa::gui_language::Language::ZhCn);
    EXPECT_EQ(wa::ui_text::pipelineCallLogEmptyText(false, false),
              wa::ui_text::get(wa::ui_text::kPipelineCallLogEmpty));
    EXPECT_NE(wa::ui_text::get(wa::ui_text::kPipelineCallLogEmpty),
              wa::ui_text::kPipelineCallLogEmpty);
    EXPECT_EQ(wa::ui_text::pipelineCallLogEmptyText(false, true),
              wa::ui_text::pipelineCallLogEmptyText(false, false));
    EXPECT_EQ(wa::ui_text::pipelineCallLogEmptyText(true, false),
              wa::ui_text::get(wa::ui_text::kPipelineCallLogWaiting));
    EXPECT_EQ(wa::ui_text::pipelineCallLogEmptyText(true, true),
              wa::ui_text::get(wa::ui_text::kPipelineCallLogWaitingPump));
    EXPECT_EQ(wa::ui_text::channelPhraseText(wa::ChannelPhrase::None),
              wa::ui_text::get(wa::ui_text::kRenderNoPhrase));
    EXPECT_STREQ(wa::ui_text::channelPhraseText(wa::ChannelPhrase::Channel1), "channel 1");

    wa::LiveSessionView row;
    row.flow = wa::PipelineFlow::Render;
    row.sessionMute = false;
    row.processName = "chrome.exe";
    row.state = "Active";
    const std::string tip = wa::ui_text::formatLiveSessionTooltip(row);
    const char* flow = wa::ui_text::get(wa::ui_text::kPipelineFlowRender);
    const char* mute = wa::ui_text::get(wa::ui_text::kPipelineMuteNo);
    EXPECT_NE(flow, wa::ui_text::kPipelineFlowRender);
    EXPECT_NE(mute, wa::ui_text::kPipelineMuteNo);
    EXPECT_NE(tip.find(flow), std::string::npos);
    EXPECT_NE(tip.find(std::string("\nmute: ") + mute), std::string::npos);
    EXPECT_NE(tip.find("chrome.exe"), std::string::npos);
    EXPECT_NE(tip.find("Active"), std::string::npos);
    EXPECT_NE(tip.find("\nprocess: "), std::string::npos);
}

TEST_F(GuiLanguageText, MonitorLeftoverRowsEnglishPointerAndChineseSlot) {
    EXPECT_STREQ(wa::ui_text::kMonitorTab, "Monitor");
    EXPECT_STREQ(wa::ui_text::kCaptureDevice, "Capture device");
    EXPECT_STREQ(wa::ui_text::kRenderDevice, "Render device");
    EXPECT_STREQ(wa::ui_text::kNoDevices, "(no devices)");
    EXPECT_STREQ(wa::ui_text::kDelayMs, "Delay (ms)");
    EXPECT_STREQ(wa::ui_text::kWasapiShared, "WASAPI-Shared");
    EXPECT_STREQ(wa::ui_text::kWasapiExclusive, "WASAPI-Exclusive");
    EXPECT_STREQ(wa::ui_text::kSyncPlayback, "Sync playback");
    EXPECT_STREQ(wa::ui_text::kFormatDefault, " (default)");
    EXPECT_STREQ(wa::ui_text::kAudioParametersAdvanced, "Audio parameters (advanced)");
    EXPECT_STREQ(wa::ui_text::kRunningParamsReadOnly,
                 "Running: parameters are read-only. Stop the monitor to change them.");
    EXPECT_STREQ(wa::ui_text::kStreamOptionCaption, "Stream option");
    EXPECT_STREQ(wa::ui_text::kBufferMs, "Buffer (ms)");
    EXPECT_STREQ(wa::ui_text::kCategoryOther, "Other");
    EXPECT_STREQ(wa::ui_text::kCategoryCommunications, "Communications");
    EXPECT_STREQ(wa::ui_text::kCategoryMedia, "Media");
    EXPECT_STREQ(wa::ui_text::kCategoryMovie, "Movie");
    EXPECT_STREQ(wa::ui_text::kCategoryGameChat, "Game chat");
    EXPECT_STREQ(wa::ui_text::kCategorySpeech, "Speech");
    EXPECT_STREQ(wa::ui_text::kCategorySoundEffects, "Sound effects");
    EXPECT_STREQ(wa::ui_text::kCategoryGameMedia, "Game media");
    EXPECT_EQ(wa::ui_text::kAudioCategoryCount, 8);
    EXPECT_EQ(wa::ui_text::kAudioCategoryLabels[0], wa::ui_text::kCategoryOther);
    EXPECT_EQ(wa::ui_text::kAudioCategoryLabels[7], wa::ui_text::kCategoryGameMedia);
    EXPECT_STREQ(wa::ui_text::kOriginMix, "Mix");
    EXPECT_STREQ(wa::ui_text::kOriginDevice, "Device");
    EXPECT_STREQ(wa::ui_text::kOriginOem, "OEM");
    EXPECT_STREQ(wa::ui_text::kOriginStandard, "Standard");
    EXPECT_STREQ(wa::ui_text::kSources, "Sources");
    EXPECT_STREQ(wa::ui_text::kShared, "Shared");
    EXPECT_STREQ(wa::ui_text::kExclusive, "Exclusive");
    EXPECT_STREQ(wa::ui_text::kSupportExact, "Exact");
    EXPECT_STREQ(wa::ui_text::kSupportClosest, "Closest");
    EXPECT_STREQ(wa::ui_text::kMonitorCaptureDevice, "Monitor capture device");
    EXPECT_STREQ(wa::ui_text::kSystemLoopbackRenderDevice, "System Loopback render device");
    EXPECT_STREQ(wa::ui_text::kApplicationLoopbackFormatReference,
                 "Application Loopback format reference");
    EXPECT_STREQ(wa::ui_text::kCaptureWaveformSpectrogram, "Capture waveform + spectrogram");
    EXPECT_STREQ(wa::ui_text::kRenderWaveformSpectrogramDelayed,
                 "Render waveform + spectrogram (delayed)");
    EXPECT_STREQ(wa::ui_text::kCaptureChannelsShown,
                 "Capture channels: %u / %u channels shown");
    EXPECT_STREQ(wa::ui_text::kCaptureChannelWaveform, "Capture Ch %u waveform");
    EXPECT_STREQ(wa::ui_text::kCaptureChannelSpectrogram, "Capture Ch %u spectrogram");

    wa::ui_text::setLanguage(wa::gui_language::Language::En);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kSyncPlayback), wa::ui_text::kSyncPlayback);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kRunningParamsReadOnly),
              wa::ui_text::kRunningParamsReadOnly);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kCategoryMovie), wa::ui_text::kCategoryMovie);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kMonitorTab), wa::ui_text::kMonitorTab);

    wa::ui_text::setLanguage(wa::gui_language::Language::ZhCn);
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kSyncPlayback), "同步播放");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kCaptureDevice), "Capture 设备");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kNoDevices), "（无设备）");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kAudioParametersAdvanced), "音频参数（高级）");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kRunningParamsReadOnly),
                 "运行中：参数只读。请先停止 Monitor 再修改。");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kCategoryOther), "其他");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kCategoryMovie), "电影");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kSupportExact), "精确");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kSupportClosest), "最接近");
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kMonitorTab), wa::ui_text::kMonitorTab);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kWasapiShared), wa::ui_text::kWasapiShared);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kWasapiExclusive), wa::ui_text::kWasapiExclusive);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kCategoryCommunications),
              wa::ui_text::kCategoryCommunications);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kCategoryMedia), wa::ui_text::kCategoryMedia);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kShared), wa::ui_text::kShared);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kExclusive), wa::ui_text::kExclusive);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kOriginMix), wa::ui_text::kOriginMix);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kOriginOem), wa::ui_text::kOriginOem);
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kCaptureChannelWaveform), "Capture Ch %u 波形");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kCaptureChannelSpectrogram),
                 "Capture Ch %u 声谱图");
}

TEST_F(GuiLanguageText, CapabilityOriginAndSupportWordsFollowLanguage) {
    using wa::FormatOrigin;
    using wa::SupportLevel;
    const wa::FormatOrigins mixDevice =
        wa::formatOriginBit(FormatOrigin::Mix) | wa::formatOriginBit(FormatOrigin::Device);

    wa::ui_text::setLanguage(wa::gui_language::Language::En);
    EXPECT_EQ(wa::ui_text::formatOriginsLabel(0), "-");
    EXPECT_EQ(wa::ui_text::formatOriginsLabel(mixDevice), "Mix, Device");
    EXPECT_EQ(wa::ui_text::supportLabel(SupportLevel::Exact), wa::ui_text::kSupportExact);
    EXPECT_EQ(wa::ui_text::supportLabel(SupportLevel::ClosestMatch),
              wa::ui_text::kSupportClosest);
    EXPECT_STREQ(wa::ui_text::supportLabel(SupportLevel::Unsupported), "-");

    wa::ui_text::setLanguage(wa::gui_language::Language::ZhCn);
    const std::string origins = wa::ui_text::formatOriginsLabel(mixDevice);
    EXPECT_NE(origins.find(wa::ui_text::get(wa::ui_text::kOriginMix)), std::string::npos);
    EXPECT_NE(origins.find(wa::ui_text::get(wa::ui_text::kOriginDevice)), std::string::npos);
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kOriginDevice), "设备");
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kOriginMix), wa::ui_text::kOriginMix);
    EXPECT_EQ(wa::ui_text::supportLabel(SupportLevel::Exact),
              wa::ui_text::get(wa::ui_text::kSupportExact));
    EXPECT_NE(wa::ui_text::get(wa::ui_text::kSupportExact), wa::ui_text::kSupportExact);
}

TEST_F(GuiLanguageText, LoopbackLeftoverRowsEnglishPointerAndChineseSlot) {
    EXPECT_STREQ(wa::ui_text::kLoopbackTab, "Loopback");
    EXPECT_STREQ(wa::ui_text::kNoRenderDevices, "(no render devices)");
    EXPECT_STREQ(wa::ui_text::kSystemAudio, "System audio");
    EXPECT_STREQ(wa::ui_text::kApplicationLoopbackNoSessions, "(none)");

    wa::ui_text::setLanguage(wa::gui_language::Language::En);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kLoopbackTab), wa::ui_text::kLoopbackTab);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kNoRenderDevices), wa::ui_text::kNoRenderDevices);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kSystemAudio), wa::ui_text::kSystemAudio);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kApplicationLoopbackNoSessions),
              wa::ui_text::kApplicationLoopbackNoSessions);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kSource), wa::ui_text::kSource);
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kIdle), wa::ui_text::kIdle);

    wa::ui_text::setLanguage(wa::gui_language::Language::ZhCn);
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kLoopbackTab), "Loopback");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kNoRenderDevices), "（无 Render 设备）");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kSystemAudio), "系统音频");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kApplicationLoopbackNoSessions), "（无）");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kSource), "来源");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kControl), "控制");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kRefreshDevices), "刷新设备");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kIdle), "空闲");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kRunning), "运行中");
    EXPECT_STREQ(wa::ui_text::get(wa::ui_text::kError), "错误");
    EXPECT_EQ(wa::ui_text::get(wa::ui_text::kLogExpand), wa::ui_text::kLogExpand);
}
