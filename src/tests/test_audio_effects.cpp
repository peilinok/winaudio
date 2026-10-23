#include <gtest/gtest.h>
#include "AudioEffects.h"

using namespace wa;

namespace {

GUID makeGuid(unsigned long d1, unsigned short d2, unsigned short d3,
              unsigned char b0, unsigned char b1, unsigned char b2, unsigned char b3,
              unsigned char b4, unsigned char b5, unsigned char b6, unsigned char b7) {
    GUID g{};
    g.Data1 = d1;
    g.Data2 = d2;
    g.Data3 = d3;
    g.Data4[0] = b0; g.Data4[1] = b1; g.Data4[2] = b2; g.Data4[3] = b3;
    g.Data4[4] = b4; g.Data4[5] = b5; g.Data4[6] = b6; g.Data4[7] = b7;
    return g;
}

}  // namespace

TEST(AudioEffects, StateName) {
    EXPECT_STREQ(audioEffectStateName(AudioEffectState::Unknown), "unknown");
    EXPECT_STREQ(audioEffectStateName(AudioEffectState::Off), "off");
    EXPECT_STREQ(audioEffectStateName(AudioEffectState::On), "on");
}

TEST(AudioEffects, StateFromWin32) {
    EXPECT_EQ(audioEffectStateFromWin32(0), AudioEffectState::Off);
    EXPECT_EQ(audioEffectStateFromWin32(1), AudioEffectState::On);
    EXPECT_EQ(audioEffectStateFromWin32(2), AudioEffectState::Unknown);
    EXPECT_EQ(audioEffectStateFromWin32(99), AudioEffectState::Unknown);
}

TEST(AudioEffects, StateToWin32) {
    uint32_t v = 99;
    EXPECT_TRUE(audioEffectStateToWin32(AudioEffectState::Off, v));
    EXPECT_EQ(v, 0u);
    EXPECT_TRUE(audioEffectStateToWin32(AudioEffectState::On, v));
    EXPECT_EQ(v, 1u);
    EXPECT_FALSE(audioEffectStateToWin32(AudioEffectState::Unknown, v));
}

TEST(AudioEffects, StateFromName) {
    EXPECT_EQ(audioEffectStateFromName("on"), AudioEffectState::On);
    EXPECT_EQ(audioEffectStateFromName("ON"), AudioEffectState::On);
    EXPECT_EQ(audioEffectStateFromName("off"), AudioEffectState::Off);
    EXPECT_EQ(audioEffectStateFromName("Off"), AudioEffectState::Off);
    EXPECT_EQ(audioEffectStateFromName("maybe"), AudioEffectState::Unknown);
    EXPECT_EQ(audioEffectStateFromName(""), AudioEffectState::Unknown);
}

TEST(AudioEffects, TypeNameKnownGuids) {
    // AUDIO_EFFECT_TYPE_BASS_BOOST
    const GUID bass = makeGuid(0x6f64adc5, 0x8211, 0x11e2,
                               0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa);
    EXPECT_EQ(audioEffectTypeName(bass), "Bass Boost");

    // AUDIO_EFFECT_TYPE_NOISE_SUPPRESSION
    const GUID ns = makeGuid(0x6f64adbf, 0x8211, 0x11e2,
                             0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa);
    EXPECT_EQ(audioEffectTypeName(ns), "Noise Suppression");

    // AUDIO_EFFECT_TYPE_DEEP_NOISE_SUPPRESSION
    const GUID dns = makeGuid(0x6f64add0, 0x8211, 0x11e2,
                              0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa);
    EXPECT_EQ(audioEffectTypeName(dns), "Deep Noise Suppression");

    // AUDIO_EFFECT_TYPE_ACOUSTIC_ECHO_CANCELLATION
    const GUID aec = makeGuid(0x6f64adbe, 0x8211, 0x11e2,
                              0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa);
    EXPECT_EQ(audioEffectTypeName(aec), "Acoustic Echo Cancellation");
}

TEST(AudioEffects, TypeNameUnknownFallsBackToBracedGuid) {
    const GUID unknown = makeGuid(0x11111111, 0x2222, 0x3333,
                                  0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb);
    const std::string name = audioEffectTypeName(unknown);
    EXPECT_EQ(name, guidToBracedString(unknown));
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(name.front(), '{');
    EXPECT_EQ(name.back(), '}');
}

TEST(AudioEffects, ParseGuidBraced) {
    GUID g{};
    EXPECT_TRUE(parseGuidBraced("{6F64ADBF-8211-11E2-8C70-2C27D7F001FA}", g));
    EXPECT_EQ(audioEffectTypeName(g), "Noise Suppression");

    GUID g2{};
    EXPECT_TRUE(parseGuidBraced("6f64adbf-8211-11e2-8c70-2c27d7f001fa", g2));
    EXPECT_TRUE(InlineIsEqualGUID(g, g2));

    GUID bad{};
    EXPECT_FALSE(parseGuidBraced("not-a-guid", bad));
    EXPECT_FALSE(parseGuidBraced("", bad));
}

TEST(AudioEffects, ResolveByNameAndSlug) {
    GUID g{};
    EXPECT_TRUE(resolveAudioEffectId("Noise Suppression", g));
    EXPECT_EQ(audioEffectTypeName(g), "Noise Suppression");

    GUID g2{};
    EXPECT_TRUE(resolveAudioEffectId("noise-suppression", g2));
    EXPECT_TRUE(InlineIsEqualGUID(g, g2));

    GUID g3{};
    EXPECT_TRUE(resolveAudioEffectId("NOISE_SUPPRESSION", g3));
    EXPECT_TRUE(InlineIsEqualGUID(g, g3));

    GUID g4{};
    EXPECT_TRUE(resolveAudioEffectId("{6F64ADBF-8211-11E2-8C70-2C27D7F001FA}", g4));
    EXPECT_TRUE(InlineIsEqualGUID(g, g4));

    GUID miss{};
    EXPECT_FALSE(resolveAudioEffectId("Totally Fake Effect", miss));
    EXPECT_FALSE(resolveAudioEffectId("", miss));
}

TEST(AudioEffects, StreamEffectDefaults) {
    StreamAudioEffect e{};
    EXPECT_TRUE(e.id.empty());
    EXPECT_TRUE(e.typeName.empty());
    EXPECT_FALSE(e.canSetState);
    EXPECT_EQ(e.state, AudioEffectState::Unknown);
}

TEST(AudioEffects, ProbeDefaultDeviceSkipsGracefully) {
    // Hardware / OS dependent: Win10 or missing manager yields Ok + empty;
    // Win11 may return effects. Never fail the suite for absence.
    std::vector<StreamAudioEffect> effects;
    const Result r = probeStreamAudioEffects(DataFlow::Render, DeviceId{}, effects);
    if (!r) {
        GTEST_SKIP() << "probe unavailable: " << r.message;
    }
    // Ok path: list may be empty (Win10 / no FX) or populated (Win11+).
    for (const auto& e : effects) {
        EXPECT_FALSE(e.typeName.empty());
        EXPECT_FALSE(e.id.empty());
        EXPECT_TRUE(e.state == AudioEffectState::Off || e.state == AudioEffectState::On ||
                    e.state == AudioEffectState::Unknown);
    }
}

TEST(AudioEffects, SetUnknownTokenFailsCleanly) {
    const Result r = setStreamAudioEffect(DataFlow::Render, DeviceId{},
                                          "Totally Fake Effect", AudioEffectState::Off);
    EXPECT_FALSE(r);
    EXPECT_NE(r.message.find("unknown effect"), std::string::npos);
}

TEST(AudioEffects, SetNullClientFailsCleanly) {
    GUID ns{};
    ASSERT_TRUE(resolveAudioEffectId("noise-suppression", ns));
    const Result nullClient = setAudioEffectStateOnClient(nullptr, ns, AudioEffectState::On);
    EXPECT_FALSE(nullClient);
    EXPECT_NE(nullClient.message.find("null"), std::string::npos);
}

TEST(AudioEffects, SetHardwareSkipsIfUnavailable) {
    // Prefer a settable effect if the probe finds one; otherwise skip.
    std::vector<StreamAudioEffect> effects;
    const Result pr = probeStreamAudioEffects(DataFlow::Capture, DeviceId{}, effects);
    if (!pr) {
        GTEST_SKIP() << "probe unavailable: " << pr.message;
    }
    const StreamAudioEffect* target = nullptr;
    for (const auto& e : effects) {
        if (e.canSetState) {
            target = &e;
            break;
        }
    }
    if (!target) {
        GTEST_SKIP() << "no canSetState effect on default capture";
    }

    const AudioEffectState original = target->state;
    const AudioEffectState flip =
        (original == AudioEffectState::On) ? AudioEffectState::Off : AudioEffectState::On;

    std::vector<StreamAudioEffect> after;
    Result r = setStreamAudioEffect(DataFlow::Capture, DeviceId{}, target->id, flip, &after);
    if (!r) {
        GTEST_SKIP() << "set unavailable: " << r.message;
    }

    bool found = false;
    for (const auto& e : after) {
        if (e.id == target->id) {
            found = true;
            EXPECT_EQ(e.state, flip);
            break;
        }
    }
    EXPECT_TRUE(found);

    // Restore best-effort.
    (void)setStreamAudioEffect(DataFlow::Capture, DeviceId{}, target->id, original, nullptr);
}
