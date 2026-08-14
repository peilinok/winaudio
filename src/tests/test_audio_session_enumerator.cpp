#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "AudioSessionEnumerator.h"
#include "ApplicationLoopbackUiModel.h"
#include "AppUiText.h"

using namespace wa;

TEST(AudioSessionEnumerator, SortsByProcessNameThenPid) {
    std::vector<AudioSessionProcess> rows = {
        {300u, L"zoom.exe"},
        {200u, L"chrome.exe"},
        {100u, L"chrome.exe"},
    };

    sortAndDedupeAudioSessionProcesses(rows);

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].processName, L"chrome.exe");
    EXPECT_EQ(rows[0].processId, 100u);
    EXPECT_EQ(rows[1].processName, L"chrome.exe");
    EXPECT_EQ(rows[1].processId, 200u);
    EXPECT_EQ(rows[2].processName, L"zoom.exe");
    EXPECT_EQ(rows[2].processId, 300u);
}

TEST(AudioSessionEnumerator, DeduplicatesByPid) {
    std::vector<AudioSessionProcess> rows = {
        {200u, L"chrome.exe"},
        {100u, L"msedge.exe"},
        {200u, L"chrome.exe"},
        {100u, L"msedge.exe"},
    };

    sortAndDedupeAudioSessionProcesses(rows);

    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].processName, L"chrome.exe");
    EXPECT_EQ(rows[0].processId, 200u);
    EXPECT_EQ(rows[1].processName, L"msedge.exe");
    EXPECT_EQ(rows[1].processId, 100u);
}

TEST(AppLoopbackPid, ParseAcceptsManualPid) {
    uint32_t pid = 0;
    EXPECT_TRUE(parseApplicationLoopbackPid("4242", pid));
    EXPECT_EQ(pid, 4242u);
}

TEST(AppLoopbackPid, ParseRejectsZeroAndGarbage) {
    uint32_t pid = 123u;

    EXPECT_FALSE(parseApplicationLoopbackPid("0", pid));
    EXPECT_EQ(pid, 123u);
    EXPECT_FALSE(parseApplicationLoopbackPid("abc", pid));
    EXPECT_EQ(pid, 123u);
    EXPECT_FALSE(parseApplicationLoopbackPid("42x", pid));
    EXPECT_EQ(pid, 123u);
    EXPECT_FALSE(parseApplicationLoopbackPid("-1", pid));
    EXPECT_EQ(pid, 123u);
    EXPECT_FALSE(parseApplicationLoopbackPid("+1", pid));
    EXPECT_EQ(pid, 123u);
}

TEST(AppLoopbackUiText, ExposesRequiredControls) {
    EXPECT_STREQ(wa::ui_text::kApplicationLoopbackTab, "Application Loopback");
    EXPECT_STREQ(wa::ui_text::kApplicationLoopbackRefresh, "Refresh");
    EXPECT_STREQ(wa::ui_text::kApplicationLoopbackPidLabel, "PID");
    EXPECT_STREQ(wa::ui_text::kApplicationLoopbackExclude, "Exclude");
    EXPECT_STREQ(wa::ui_text::kApplicationLoopbackSessions, "Sessions");
}

TEST(AdvancedOptionsUiText, ExposesExplicitClientPropertiesControls) {
    EXPECT_STREQ(wa::ui_text::kAdvancedCaptureSection, "Capture");
    EXPECT_STREQ(wa::ui_text::kAdvancedRenderSection, "Render");
    EXPECT_STREQ(wa::ui_text::kAdvancedSetClientProperties, "Set client properties");
    EXPECT_FALSE(wa::ui_text::kAdvancedSetClientPropertiesDefault);
    EXPECT_STREQ(wa::ui_text::kAdvancedHardwareOffload, "Hardware offload");
    EXPECT_EQ(wa::ui_text::kAdvancedStreamOptionCount, 5);
    EXPECT_STREQ(wa::ui_text::kAdvancedStreamOptions[0], "None");
    EXPECT_STREQ(wa::ui_text::kAdvancedStreamOptions[4], "Post-volume loopback");
    EXPECT_NE(std::string(wa::ui_text::kAdvancedOptionsHelp).find("only when enabled"),
              std::string::npos);
}

TEST(ChartsFreezeUiText, ExposesPauseResumeAndPausedLabels) {
    EXPECT_STREQ(wa::ui_text::kChartsPause, "Pause charts");
    EXPECT_STREQ(wa::ui_text::kChartsResume, "Resume charts");
    EXPECT_STREQ(wa::ui_text::kChartsPaused, "PAUSED");
}

TEST(ChartsTimeZoomUiText, ExposesZoomAndResetLabels) {
    EXPECT_STREQ(wa::ui_text::kChartsZoomOut, "Zoom out");
    EXPECT_STREQ(wa::ui_text::kChartsZoomIn, "Zoom in");
    EXPECT_STREQ(wa::ui_text::kChartsResetView, "Reset view");
    EXPECT_STREQ(wa::ui_text::kLoopbackCreate, "Create Track");
    EXPECT_STREQ(wa::ui_text::kLoopbackDestroy, "Destroy");
    EXPECT_STREQ(wa::ui_text::kLoopbackDestroyAll, "Destroy all");
    EXPECT_STREQ(wa::ui_text::kLoopbackTracks, "Tracks");
    EXPECT_STREQ(wa::ui_text::kLoopbackSilentRender, "Silent render keepalive");
    EXPECT_STREQ(wa::ui_text::kLoopbackWavOptional, "WAV (optional)");
    EXPECT_STREQ(wa::ui_text::kLoopbackFormatOptional, "Format (optional)");
    EXPECT_STREQ(wa::ui_text::kLoopbackEmptyHint, "Create a Track to capture system audio.");
}

TEST(AppLoopbackUiModel, SelectingRowCopiesPidIntoEditableBuffer) {
    std::vector<AudioSessionProcess> rows = {
        {111u, L"one.exe"},
        {222u, L"two.exe"},
    };
    char pid[16] = "999";

    EXPECT_TRUE(wa::app_loopback_ui::copySessionPidToBuffer(rows, 1, pid, sizeof(pid)));

    EXPECT_STREQ(pid, "222");
}

TEST(AppLoopbackUiModel, InvalidSelectionDoesNotOverwriteManualPid) {
    std::vector<AudioSessionProcess> rows = {{111u, L"one.exe"}};
    char pid[16] = "999";

    EXPECT_FALSE(wa::app_loopback_ui::copySessionPidToBuffer(rows, 3, pid, sizeof(pid)));

    EXPECT_STREQ(pid, "999");
}

TEST(AppLoopbackUiModel, RefreshFailureStillMarksLoaded) {
    std::vector<AudioSessionProcess> rows = {{111u, L"one.exe"}};
    std::vector<AudioSessionProcess> refreshed = {{222u, L"two.exe"}};
    bool loaded = false;
    int selected = 0;

    wa::app_loopback_ui::applyRefreshResult(false, rows, loaded, selected,
                                            std::move(refreshed));

    EXPECT_TRUE(loaded);
    EXPECT_TRUE(rows.empty());
    EXPECT_EQ(selected, -1);
}
