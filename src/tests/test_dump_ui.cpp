#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <gtest/gtest.h>
#include <windows.h>
#include "DumpUi.h"
#include "AppUiText.h"

TEST(DumpUi, FolderPickerTakeEmptyWhenIdle) {
    wa::dump_ui::FolderPicker p;
    EXPECT_FALSE(p.busy());
    std::wstring folder;
    bool accepted = true;
    EXPECT_FALSE(p.take(folder, accepted));
}

TEST(DumpUi, DownloadsFolderIsNonEmpty) {
    EXPECT_FALSE(wa::dump_ui::downloadsFolder().empty());
}

TEST(DumpUi, FinalizingDisablesNewDumpAndOnlySuccessfulGenerationsRevealOnce) {
    wa::dump_ui::Completion completion;
    wa::WavSinkStatus status;
    status.generation = 7;
    status.phase = wa::WavDumpPhase::Accepting;
    EXPECT_FALSE(wa::dump_ui::canStart(status, true, false));
    EXPECT_FALSE(completion.take(status));
    status.phase = wa::WavDumpPhase::Finalizing;
    EXPECT_FALSE(wa::dump_ui::canStart(status, true, false));
    EXPECT_FALSE(completion.take(status));
    status.phase = wa::WavDumpPhase::Success;
    EXPECT_TRUE(completion.take(status));
    EXPECT_FALSE(completion.take(status));
    EXPECT_TRUE(wa::dump_ui::canStart(status, true, false));
    EXPECT_FALSE(wa::dump_ui::canStart(status, false, false));
    EXPECT_FALSE(wa::dump_ui::canStart(status, true, true));
    ++status.generation;
    status.phase = wa::WavDumpPhase::Preparing;
    EXPECT_FALSE(completion.take(status));
    auto stale = status;
    --stale.generation;
    stale.phase = wa::WavDumpPhase::Success;
    EXPECT_FALSE(completion.take(stale));
    status.phase = wa::WavDumpPhase::Cancelled;
    EXPECT_FALSE(completion.take(status));
    ++status.generation;
    status.phase = wa::WavDumpPhase::Failure;
    EXPECT_FALSE(completion.take(status));
    ++status.generation;
    status.phase = wa::WavDumpPhase::Success;
    EXPECT_TRUE(completion.take(status));
    EXPECT_FALSE(completion.take(status));
}

TEST(DumpUi, EveryDumpPhaseHasEnglishAndChineseText) {
    const wa::WavDumpPhase phases[]{wa::WavDumpPhase::Preparing, wa::WavDumpPhase::Accepting,
        wa::WavDumpPhase::Finalizing, wa::WavDumpPhase::Success,
        wa::WavDumpPhase::Failure, wa::WavDumpPhase::Cancelled};
    const char* en[]{"Preparing dump", "Recording", "Finalizing dump", "Dump completed",
                     "Dump failed", "Dump cancelled"};
    const char* zh[]{"正在准备", "正在接收", "正在收尾", "文件已完成", "文件失败", "已取消"};
    for (size_t i = 0; i < 6; ++i) {
        wa::ui_text::setLanguage(wa::gui_language::Language::En);
        EXPECT_STREQ(wa::dump_ui::statusText(phases[i]), en[i]);
        wa::ui_text::setLanguage(wa::gui_language::Language::ZhCn);
        EXPECT_STREQ(wa::dump_ui::statusText(phases[i]), zh[i]);
    }
    wa::ui_text::setLanguage(wa::gui_language::Language::En);
}

TEST(DumpUi, RegistryRoundTripDoesNotRewriteBadPath) {
    const std::wstring prev = wa::dump_ui::storedDumpFolder();
    const std::wstring good = L"C:\\Windows";
    wa::dump_ui::saveDumpFolder(good);
    EXPECT_EQ(wa::dump_ui::storedDumpFolder(), good);
    EXPECT_EQ(wa::dump_ui::loadDumpFolder(), good);

    const std::wstring missing = L"Z:\\wa_dump_folder_does_not_exist";
    wa::dump_ui::saveDumpFolder(missing);
    EXPECT_EQ(wa::dump_ui::storedDumpFolder(), missing);
    EXPECT_EQ(wa::dump_ui::loadDumpFolder(), wa::dump_ui::downloadsFolder());
    EXPECT_EQ(wa::dump_ui::storedDumpFolder(), missing);

    if (prev.empty()) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\WinAudio", 0, KEY_SET_VALUE, &key)
            == ERROR_SUCCESS) {
            RegDeleteValueW(key, L"DumpFolder");
            RegCloseKey(key);
        }
    } else {
        wa::dump_ui::saveDumpFolder(prev);
    }
}
