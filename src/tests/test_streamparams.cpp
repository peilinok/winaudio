#include <gtest/gtest.h>
#include "StreamParams.h"
#include "WasapiStream.h"
using namespace wa;

TEST(StreamParams, DefaultIsAllDefault) {
    StreamParams p;
    EXPECT_TRUE(p.isDefault());
    EXPECT_FALSE(p.anyClientProps());
    EXPECT_EQ(p.bufferMs, 0u);
}

TEST(StreamParams, Predicates) {
    StreamParams a; a.category = AudioCategory::Communications;
    EXPECT_TRUE(a.anyClientProps()); EXPECT_FALSE(a.isDefault());
    StreamParams b; b.option = StreamOption::Raw;
    EXPECT_TRUE(b.anyClientProps()); EXPECT_FALSE(b.isDefault());
    StreamParams c; c.offload = OffloadMode::Force;
    EXPECT_TRUE(c.anyClientProps()); EXPECT_FALSE(c.isDefault());
    StreamParams d; d.ducking = DuckingMode::OptOut;      // ducking 不属于 client-props
    EXPECT_FALSE(d.anyClientProps()); EXPECT_FALSE(d.isDefault());
    StreamParams e; e.bufferMs = 50;                       // bufferMs 也不属于
    EXPECT_FALSE(e.anyClientProps()); EXPECT_FALSE(e.isDefault());
}

TEST(StreamParams, CategoryMapping) {
    EXPECT_EQ(mapCategory(AudioCategory::Other),          AudioCategory_Other);
    EXPECT_EQ(mapCategory(AudioCategory::Communications), AudioCategory_Communications);
    EXPECT_EQ(mapCategory(AudioCategory::Media),          AudioCategory_Media);
    EXPECT_EQ(mapCategory(AudioCategory::Movie),          AudioCategory_Movie);
    EXPECT_EQ(mapCategory(AudioCategory::GameChat),       AudioCategory_GameChat);
    EXPECT_EQ(mapCategory(AudioCategory::Speech),         AudioCategory_Speech);
    EXPECT_EQ(mapCategory(AudioCategory::SoundEffects),   AudioCategory_SoundEffects);
    EXPECT_EQ(mapCategory(AudioCategory::GameMedia),      AudioCategory_GameMedia);
    EXPECT_EQ(mapCategory(AudioCategory::Default),        AudioCategory_Other); // placeholder when props set w/o category
}

TEST(StreamParams, OptionMapping) {
    EXPECT_EQ(mapStreamOption(StreamOption::Default),     AUDCLNT_STREAMOPTIONS_NONE);
    EXPECT_EQ(mapStreamOption(StreamOption::Raw),         AUDCLNT_STREAMOPTIONS_RAW);
    EXPECT_EQ(mapStreamOption(StreamOption::MatchFormat), AUDCLNT_STREAMOPTIONS_MATCH_FORMAT);
}

TEST(StreamParams, ExclusiveRejectsAdvancedParams) {
    // open() validates synchronously (no hardware touched: activation is deferred to start()).
    WasapiCaptureStream s(WasapiMode::Exclusive, nullptr);
    StreamParams raw; raw.option = StreamOption::Raw;
    EXPECT_FALSE(s.open(L"", AudioFormat{}, nullptr, raw));
    StreamParams duck; duck.ducking = DuckingMode::OptOut;
    EXPECT_FALSE(s.open(L"", AudioFormat{}, nullptr, duck));
    StreamParams onlyBuf; onlyBuf.bufferMs = 50;           // bufferMs alone IS allowed in exclusive
    EXPECT_TRUE(s.open(L"", AudioFormat{}, nullptr, onlyBuf));
    WasapiCaptureStream sh(WasapiMode::Shared, nullptr);   // shared accepts advanced params
    EXPECT_TRUE(sh.open(L"", AudioFormat{}, nullptr, raw));
}
