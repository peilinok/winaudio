#include <gtest/gtest.h>
#include "LogRegionPrefs.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeCaseDir() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    fs::path dir = fs::temp_directory_path() / "wa_log_region_prefs" / info->name();
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

}  // namespace

TEST(LogRegionPrefs, FileNameIsWinaudioUiIni) {
    EXPECT_STREQ(wa::log_region_prefs::kFileName, "winaudio.ui.ini");
}

TEST(LogRegionPrefs, MissingPathIsExpandedAndNotInvalid) {
    const fs::path missing = makeCaseDir() / "no-such.ini";
    const auto r = wa::log_region_prefs::load(missing);
    EXPECT_FALSE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Missing);
}

TEST(LogRegionPrefs, EmptyFileIsInvalidExpanded) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    writeFile(file, "");
    const auto r = wa::log_region_prefs::load(file);
    EXPECT_FALSE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Invalid);
}

TEST(LogRegionPrefs, GarbageContentsAreInvalidExpanded) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    writeFile(file, "not a preference file\nfoo=bar\n");
    const auto r = wa::log_region_prefs::load(file);
    EXPECT_FALSE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Invalid);
}

TEST(LogRegionPrefs, ValueOtherThanZeroOrOneIsInvalidExpanded) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    writeFile(file, "log_collapsed=true\n");
    const auto r = wa::log_region_prefs::load(file);
    EXPECT_FALSE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Invalid);
}

TEST(LogRegionPrefs, ZeroMeansExpanded) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    writeFile(file, "log_collapsed=0\n");
    const auto r = wa::log_region_prefs::load(file);
    EXPECT_FALSE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Ok);
}

TEST(LogRegionPrefs, OneMeansCollapsed) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    writeFile(file, "log_collapsed=1\n");
    const auto r = wa::log_region_prefs::load(file);
    EXPECT_TRUE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Ok);
}

TEST(LogRegionPrefs, IgnoresUnknownLinesAndKeepsLastValidBit) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    writeFile(file, "noise=1\nlog_collapsed=0\nlog_collapsed=1\n");
    const auto r = wa::log_region_prefs::load(file);
    EXPECT_TRUE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Ok);
}

TEST(LogRegionPrefs, RoundTripCollapsedAndExpanded) {
    const fs::path file = makeCaseDir() / wa::log_region_prefs::kFileName;
    ASSERT_TRUE(wa::log_region_prefs::save(file, true));
    auto r = wa::log_region_prefs::load(file);
    EXPECT_TRUE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Ok);

    ASSERT_TRUE(wa::log_region_prefs::save(file, false));
    r = wa::log_region_prefs::load(file);
    EXPECT_FALSE(r.collapsed);
    EXPECT_EQ(r.kind, wa::log_region_prefs::LoadKind::Ok);
}

TEST(LogRegionPrefs, SaveToDirectoryPathReportsFailure) {
    const fs::path dir = makeCaseDir();
    EXPECT_FALSE(wa::log_region_prefs::save(dir, true));
}
