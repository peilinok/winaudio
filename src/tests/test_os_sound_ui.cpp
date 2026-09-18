#include <gtest/gtest.h>
#include "OsSoundUi.h"

TEST(OsSoundLaunchRecipe, MonitorMmsysOpensRecordingTab) {
    const auto r = wa::os_sound_ui::recipe(wa::os_sound_ui::Page::Monitor,
                                           wa::os_sound_ui::Button::Mmsys);
    EXPECT_STREQ(r.file, L"control.exe");
    EXPECT_STREQ(r.params, L"mmsys.cpl,,1");
    EXPECT_STREQ(r.label, "mmsys.cpl");
    EXPECT_STREQ(r.failureLog, "mmsys.cpl: ShellExecute failed");
}

TEST(OsSoundLaunchRecipe, LoopbackMmsysOpensPlaybackTab) {
    const auto r = wa::os_sound_ui::recipe(wa::os_sound_ui::Page::Loopback,
                                           wa::os_sound_ui::Button::Mmsys);
    EXPECT_STREQ(r.file, L"control.exe");
    EXPECT_STREQ(r.params, L"mmsys.cpl,,0");
    EXPECT_STREQ(r.label, "mmsys.cpl");
    EXPECT_STREQ(r.failureLog, "mmsys.cpl: ShellExecute failed");
}

TEST(OsSoundLaunchRecipe, MsSettingsIsTheSoundUriOnEveryPage) {
    const wa::os_sound_ui::Page pages[] = {
        wa::os_sound_ui::Page::Monitor,
        wa::os_sound_ui::Page::Loopback,
        wa::os_sound_ui::Page::ApplicationLoopback,
    };
    for (const auto page : pages) {
        const auto r = wa::os_sound_ui::recipe(page, wa::os_sound_ui::Button::MsSettings);
        EXPECT_STREQ(r.file, L"ms-settings:sound");
        EXPECT_STREQ(r.params, L"");
        EXPECT_STREQ(r.label, "ms-settings");
        EXPECT_STREQ(r.failureLog, "ms-settings: ShellExecute failed");
    }
}

TEST(OsSoundLaunchRecipe, ShellExecuteReturnAtMost32IsFailure) {
    EXPECT_TRUE(wa::os_sound_ui::shellExecuteFailed(32));
    EXPECT_TRUE(wa::os_sound_ui::shellExecuteFailed(0));
    EXPECT_TRUE(wa::os_sound_ui::shellExecuteFailed(5));
}

TEST(OsSoundLaunchRecipe, ShellExecuteReturnAbove32IsSuccess) {
    EXPECT_FALSE(wa::os_sound_ui::shellExecuteFailed(33));
    EXPECT_FALSE(wa::os_sound_ui::shellExecuteFailed(42));
}

TEST(OsSoundLaunchRecipe, ApplicationLoopbackMmsysMatchesLoopbackPlayback) {
    const auto loopback = wa::os_sound_ui::recipe(wa::os_sound_ui::Page::Loopback,
                                                 wa::os_sound_ui::Button::Mmsys);
    const auto app = wa::os_sound_ui::recipe(wa::os_sound_ui::Page::ApplicationLoopback,
                                            wa::os_sound_ui::Button::Mmsys);
    EXPECT_STREQ(app.file, L"control.exe");
    EXPECT_STREQ(app.params, L"mmsys.cpl,,0");
    EXPECT_STREQ(app.label, loopback.label);
    EXPECT_STREQ(app.failureLog, loopback.failureLog);
}
