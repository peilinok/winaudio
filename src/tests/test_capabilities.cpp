#include <algorithm>
#include <gtest/gtest.h>
#include "Capabilities.h"
#include "FormatSpec.h"
using namespace wa;

TEST(Capabilities, CandidateSpaceIs48) {
    auto c = allFormatCandidates();
    EXPECT_EQ(c.size(), 48u);
    // 含 48000/16/2 int 与 48000/32/2 float
    EXPECT_NE(std::find(c.begin(), c.end(), AudioFormat{48000,2,16,false}), c.end());
    EXPECT_NE(std::find(c.begin(), c.end(), AudioFormat{48000,2,32,true}),  c.end());
}

TEST(Capabilities, MatrixReflectsPredicates) {
    std::vector<AudioFormat> cands = {{48000,2,16,false}, {96000,2,24,false}};
    auto m = buildCapabilityMatrix(cands,
        [](const AudioFormat& f){ return f.sampleRate == 48000; },          // sharedPred
        [](const AudioFormat& f){ return f.bitsPerSample == 24; });         // exclusivePred
    ASSERT_EQ(m.size(), 2u);
    EXPECT_TRUE (m[0].sharedOk);  EXPECT_FALSE(m[0].exclusiveOk);   // 48000/16
    EXPECT_FALSE(m[1].sharedOk);  EXPECT_TRUE (m[1].exclusiveOk);   // 96000/24
}

TEST(Capabilities, DefaultSharedIsMix) {
    AudioFormat mix{44100,2,32,true};
    auto d = chooseDefaultFormat(BackendKind::WasapiShared, mix, nullptr, {}, [](const AudioFormat&){return false;});
    EXPECT_EQ(d, mix);
}

TEST(Capabilities, DefaultExclusivePrefersDeviceFormat) {
    AudioFormat dev{48000,1,16,false};
    auto d = chooseDefaultFormat(BackendKind::WasapiExclusive, AudioFormat{48000,2,32,true},
                                 &dev, defaultExclusiveCaptureCandidates(),
                                 [](const AudioFormat&){ return true; });   // 一切支持
    EXPECT_EQ(d, dev);                                                      // 首选 deviceFormat
}

TEST(Capabilities, DefaultExclusiveFallsBackWhenDeviceUnsupported) {
    AudioFormat dev{12345,7,16,false};                                      // 不在候选、pred 拒
    auto cands = defaultExclusiveCaptureCandidates();                       // 首项 48000/2/16
    auto d = chooseDefaultFormat(BackendKind::WasapiExclusive, AudioFormat{},
                                 &dev, cands,
                                 [](const AudioFormat& f){ return f == AudioFormat{48000,2,16,false}; });
    EXPECT_EQ(d, (AudioFormat{48000,2,16,false}));
}
