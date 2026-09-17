#include <algorithm>
#include <gtest/gtest.h>
#include "Capabilities.h"
#include "FormatSpec.h"
using namespace wa;

TEST(Capabilities, CandidateSpaceIncludesSurroundLayouts) {
    auto c = allFormatCandidates();
    EXPECT_EQ(c.size(), 120u);
    // 含 48000/16/2 int 与 48000/32/2 float
    EXPECT_NE(std::find(c.begin(), c.end(), AudioFormat{48000,2,16,false}), c.end());
    EXPECT_NE(std::find(c.begin(), c.end(), AudioFormat{48000,2,32,true}),  c.end());
    EXPECT_NE(std::find(c.begin(), c.end(), AudioFormat{44100,8,32,true}),  c.end());
}

TEST(Capabilities, DeviceFormatsLeadCandidatesAndMergeOrigins) {
    wa::DeviceCapabilities caps;
    caps.hasMix = true;
    caps.mixFormat = AudioFormat{44100, 8, 32, true};
    caps.mixFormat.channelMask = 0x63F;
    caps.hasDevice = true;
    caps.deviceFormat = AudioFormat{44100, 8, 16, false};
    caps.deviceFormat.channelMask = 0x63F;
    caps.hasOem = true;
    caps.oemFormat = caps.deviceFormat;

    const auto candidates = capabilityCandidates(caps);
    ASSERT_EQ(candidates.size(), 120u);
    EXPECT_EQ(candidates[0].fmt, caps.mixFormat);
    EXPECT_TRUE(hasFormatOrigin(candidates[0].origins, FormatOrigin::Mix));
    EXPECT_TRUE(hasFormatOrigin(candidates[0].origins, FormatOrigin::Standard));
    EXPECT_EQ(candidates[1].fmt, caps.deviceFormat);
    EXPECT_TRUE(hasFormatOrigin(candidates[1].origins, FormatOrigin::Device));
    EXPECT_TRUE(hasFormatOrigin(candidates[1].origins, FormatOrigin::Oem));
}

TEST(Capabilities, MatrixReflectsPredicates) {
    std::vector<FormatCandidate> cands = {
        {AudioFormat{48000,2,16,false}, formatOriginBit(FormatOrigin::Mix)},
        {AudioFormat{96000,2,24,false}, formatOriginBit(FormatOrigin::Standard)},
    };
    auto m = buildCapabilityMatrix(cands,
        [](const AudioFormat& f){ return f.sampleRate == 48000
            ? SupportLevel::Exact : SupportLevel::ClosestMatch; },
        [](const AudioFormat& f){ return f.bitsPerSample == 24
            ? SupportLevel::Exact : SupportLevel::Unsupported; });
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m[0].shared, SupportLevel::Exact);
    EXPECT_EQ(m[0].exclusive, SupportLevel::Unsupported);
    EXPECT_TRUE(hasFormatOrigin(m[0].origins, FormatOrigin::Mix));
    EXPECT_EQ(m[1].shared, SupportLevel::ClosestMatch);
    EXPECT_EQ(m[1].exclusive, SupportLevel::Exact);
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
