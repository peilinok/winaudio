#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>
#include "PhraseCatalog.h"
#include "WavFile.h"

using namespace wa;

namespace {

bool containsName(const std::vector<std::string>& names, const char* name) {
    for (const std::string& entry : names) {
        if (entry == name) return true;
    }
    return false;
}

const PhraseAsset* assetFor(ChannelPhrase phrase) {
    size_t count = 0;
    const PhraseAsset* assets = phraseAssets(count);
    for (size_t i = 0; i < count; ++i) {
        if (assets[i].phrase == phrase) return &assets[i];
    }
    return nullptr;
}

std::wstring writeTone(const std::filesystem::path& path, uint32_t rate, int16_t sample) {
    AudioFormat fmt;
    fmt.sampleRate = rate;
    fmt.channels = 1;
    fmt.bitsPerSample = 16;
    fmt.isFloat = false;
    WavWriter writer;
    EXPECT_TRUE(writer.open(path.wstring(), fmt)) << path.string();
    EXPECT_TRUE(writer.write(&sample, sizeof(sample)));
    EXPECT_TRUE(writer.close());
    return path.wstring();
}

} // namespace

TEST(ProductionCatalog, LfeFileIsNotTheRearCenterClip) {
    const PhraseAsset* lfe = assetFor(ChannelPhrase::LowFrequency);
    const PhraseAsset* rear = assetFor(ChannelPhrase::BackCenter);
    ASSERT_NE(lfe, nullptr);
    ASSERT_NE(rear, nullptr);
    EXPECT_STREQ(lfe->fileName, "LFE.wav");
    EXPECT_STREQ(rear->fileName, "Rear_Center.wav");
    EXPECT_STRNE(lfe->fileName, rear->fileName);
    EXPECT_TRUE(lfe->projectOwned);
    EXPECT_FALSE(rear->projectOwned);
}

TEST(ProductionCatalog, MissingProjectRecordingIsReportedByName) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "wa-phrase-missing";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::vector<std::string> missing = missingProductionPhrases(dir.wstring());
    EXPECT_TRUE(containsName(missing, "LFE"));
    EXPECT_TRUE(containsName(missing, "Front Left of Center"));
    EXPECT_TRUE(containsName(missing, "Front Right of Center"));
    EXPECT_TRUE(containsName(missing, "Top Center"));
    EXPECT_TRUE(containsName(missing, "Top Front Left"));
    EXPECT_TRUE(containsName(missing, "Top Front Center"));
    EXPECT_TRUE(containsName(missing, "Top Front Right"));
    EXPECT_TRUE(containsName(missing, "Top Back Left"));
    EXPECT_TRUE(containsName(missing, "Top Back Center"));
    EXPECT_TRUE(containsName(missing, "Top Back Right"));
    EXPECT_TRUE(containsName(missing, "channel 1"));
    EXPECT_TRUE(containsName(missing, "channel 8"));
    std::filesystem::remove_all(dir);
}

TEST(ProductionCatalog, WrongFormatIsReportedByNameAndAValidLfeLoads) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "wa-phrase-format";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    writeTone(dir / "LFE.wav", 48000, 1234);
    writeTone(dir / "Channel_1.wav", 44100, 50);

    const std::vector<std::string> missing = missingProductionPhrases(dir.wstring());
    EXPECT_FALSE(containsName(missing, "LFE"));
    EXPECT_TRUE(containsName(missing, "channel 1"));

    PhraseCatalog catalog;
    std::vector<std::string> reported;
    EXPECT_FALSE(loadProductionCatalog(catalog, dir.wstring(), &reported));
    const std::vector<int16_t>* lfe = catalog.find(ChannelPhrase::LowFrequency);
    ASSERT_NE(lfe, nullptr);
    ASSERT_EQ(lfe->size(), 1u);
    EXPECT_EQ((*lfe)[0], 1234);
    EXPECT_EQ(catalog.find(ChannelPhrase::Channel1), nullptr);
    EXPECT_EQ(catalog.find(ChannelPhrase::BackCenter), nullptr);
    std::filesystem::remove_all(dir);
}

TEST(ProductionCatalog, ShippedPhraseCatalogLoads) {
    const std::filesystem::path shipped(WA_CHANNEL_IDENT_DIR);
    const std::wstring dir = shipped.wstring();
    const std::vector<std::string> missing = missingProductionPhrases(dir);
    EXPECT_TRUE(missing.empty());

    PhraseCatalog catalog;
    std::vector<std::string> reported;
    EXPECT_TRUE(loadProductionCatalog(catalog, dir, &reported));
    EXPECT_TRUE(reported.empty());
    const std::vector<int16_t>* front = catalog.find(ChannelPhrase::FrontLeft);
    const std::vector<int16_t>* rear = catalog.find(ChannelPhrase::BackCenter);
    const std::vector<int16_t>* lfe = catalog.find(ChannelPhrase::LowFrequency);
    const std::vector<int16_t>* channel1 = catalog.find(ChannelPhrase::Channel1);
    const std::vector<int16_t>* topFrontLeft = catalog.find(ChannelPhrase::TopFrontLeft);
    ASSERT_NE(front, nullptr);
    ASSERT_NE(rear, nullptr);
    ASSERT_NE(lfe, nullptr);
    ASSERT_NE(channel1, nullptr);
    ASSERT_NE(topFrontLeft, nullptr);
    EXPECT_GT(front->size(), 1000u);
    EXPECT_GT(rear->size(), 1000u);
    EXPECT_GT(lfe->size(), 1000u);
    EXPECT_GT(channel1->size(), 1000u);
    EXPECT_GT(topFrontLeft->size(), 1000u);
    EXPECT_NE(*front, *rear);
    EXPECT_NE(*lfe, *rear);
    EXPECT_TRUE(std::filesystem::is_regular_file(shipped / "COPYING"));
}

TEST(RenderTrackList, InjectedCatalogStillSuppliesTheBehaviorTests) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::FrontLeft, {7, 8, 9});
    EXPECT_EQ(catalog.find(ChannelPhrase::LowFrequency), nullptr);
    const std::vector<int16_t>* clip = catalog.find(ChannelPhrase::FrontLeft);
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(*clip, (std::vector<int16_t>{7, 8, 9}));
}
