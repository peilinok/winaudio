#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <gtest/gtest.h>
#include <windows.h>
#include "DumpUi.h"

TEST(DumpUi, DownloadsFolderIsNonEmpty) {
    EXPECT_FALSE(wa::dump_ui::downloadsFolder().empty());
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
