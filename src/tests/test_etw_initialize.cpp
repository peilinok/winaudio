#include <gtest/gtest.h>
#include "EtwInitialize.h"
#include "PipelineGraph.h"

using namespace wa;

namespace {

LiveSessionView chromeCap() {
    LiveSessionView s;
    s.processId = 4242;
    s.processName = "chrome.exe";
    s.deviceId = "{0.0.1.00000000}.{cap}";
    s.deviceName = "Headset Mic";
    s.flow = PipelineFlow::Capture;
    return s;
}

EtwEventFields ev(uint32_t pid, int64_t timeMs, std::string device = {}) {
    EtwEventFields e;
    e.processId = pid;
    e.timeMs = timeMs;
    e.deviceId = std::move(device);
    return e;
}

const PipelineNode* findNode(const std::vector<PipelineNode>& nodes, const char* id) {
    for (const auto& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

bool hasParam(const PipelineNode& n, const char* key, const char* value, ObservationKind kind) {
    for (const auto& p : n.params) {
        if (p.key == key && p.value == value && p.kind == kind) return true;
    }
    return false;
}

}  // namespace

TEST(EtwInitializeMatch, DifferentPidDoesNotApplyLatestOnMachine) {
    auto latest = ev(1, 10000, "{speakers}");
    latest.hresult = 0;
    latest.category = std::string("Media");
    const auto hint = matchEtwInitialize(chromeCap(), {latest}, 10000);
    EXPECT_FALSE(hint.present);
    EXPECT_FALSE(hint.hresult.has_value());
    EXPECT_FALSE(hint.category.has_value());
}

TEST(EtwInitializeMatch, WrongDeviceSamePidStaysUnknown) {
    auto other = ev(4242, 9000, "{0.0.0.00000000}.{speakers}");
    other.hresult = 0;
    other.raw = true;
    const auto hint = matchEtwInitialize(chromeCap(), {other}, 10000);
    EXPECT_FALSE(hint.present);
    EXPECT_FALSE(hint.hresult.has_value());
}

TEST(EtwInitializeMatch, OutsideTimeWindowIsUnmatched) {
    auto old = ev(4242, 1000, "{0.0.1.00000000}.{cap}");
    old.hresult = 0;
    const auto hint = matchEtwInitialize(chromeCap(), {old}, 10000);
    EXPECT_FALSE(hint.present);
    EXPECT_FALSE(hint.hresult.has_value());
}

TEST(EtwInitializeMatch, PidTimeAndDeviceApplyDecodedFields) {
    auto hit = ev(4242, 9000, "{0.0.1.00000000}.{cap}");
    hit.category = std::string("Communications");
    hit.raw = true;
    hit.matchFormat = false;
    hit.hresult = 0;
    const auto hint = matchEtwInitialize(chromeCap(), {hit}, 10000);
    ASSERT_TRUE(hint.present);
    ASSERT_TRUE(hint.category.has_value());
    EXPECT_EQ(*hint.category, "Communications");
    ASSERT_TRUE(hint.raw.has_value());
    EXPECT_TRUE(*hint.raw);
    ASSERT_TRUE(hint.matchFormat.has_value());
    EXPECT_FALSE(*hint.matchFormat);
    ASSERT_TRUE(hint.hresult.has_value());
    EXPECT_EQ(*hint.hresult, 0);
}

TEST(EtwInitializeMatch, MissingDeviceIdStillMatchesPidAndTime) {
    auto hit = ev(4242, 9200);
    hit.category = std::string("Media");
    const auto hint = matchEtwInitialize(chromeCap(), {hit}, 10000);
    ASSERT_TRUE(hint.present);
    ASSERT_TRUE(hint.category.has_value());
    EXPECT_EQ(*hint.category, "Media");
}

TEST(EtwInitializeMatch, MergesMatchingEventsByTime) {
    auto cat = ev(4242, 8000);
    cat.category = std::string("Communications");
    auto raw = ev(4242, 8500, "{0.0.1.00000000}.{cap}");
    raw.raw = true;
    auto hr = ev(4242, 8600, "{0.0.1.00000000}.{cap}");
    hr.hresult = 0;
    const auto hint = matchEtwInitialize(chromeCap(), {hr, cat, raw}, 10000);
    ASSERT_TRUE(hint.present);
    EXPECT_EQ(*hint.category, "Communications");
    EXPECT_TRUE(*hint.raw);
    EXPECT_EQ(*hint.hresult, 0);
}

TEST(EtwInitializeMatch, FragmentDeviceIdDoesNotMatch) {
    auto hit = ev(4242, 9000, "{cap}");
    hit.hresult = 0;
    const auto hint = matchEtwInitialize(chromeCap(), {hit}, 10000);
    EXPECT_FALSE(hint.present);
}

TEST(EtwInitializeMatch, EngineEventMatchesByDeviceWithoutAppPid) {
    auto hit = ev(4, 9000, "{0.0.1.00000000}.{cap}");
    hit.raw = true;
    hit.matchFormat = false;
    const auto hint = matchEtwInitialize(chromeCap(), {hit}, 10000);
    ASSERT_TRUE(hint.present);
    ASSERT_TRUE(hint.raw.has_value());
    EXPECT_TRUE(*hint.raw);
    ASSERT_TRUE(hint.matchFormat.has_value());
    EXPECT_FALSE(*hint.matchFormat);
}

TEST(EtwInitializeMatch, EngineEventOnOtherDeviceDoesNotApply) {
    auto hit = ev(4, 9000, "{0.0.0.00000000}.{speakers}");
    hit.raw = true;
    const auto hint = matchEtwInitialize(chromeCap(), {hit}, 10000);
    EXPECT_FALSE(hint.present);
}

TEST(EtwInitializeJoin, MatchedRawSkipsSfxAsObserved) {
    auto hit = ev(4242, 9000, "{0.0.1.00000000}.{cap}");
    hit.raw = true;
    hit.category = std::string("Communications");
    hit.hresult = 0;
    const auto hint = matchEtwInitialize(chromeCap(), {hit}, 10000);
    const auto nodes = assemblePipeline(chromeCap(), {}, hint, {});
    const auto* sfx = findNode(nodes, "sfx");
    ASSERT_NE(sfx, nullptr);
    EXPECT_EQ(sfx->kind, ObservationKind::Skipped);
    EXPECT_TRUE(hasParam(*sfx, "RAW", "SFX not used", ObservationKind::Observed));
    const auto* session = findNode(nodes, "session");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(hasParam(*session, "Initialize HRESULT", "0", ObservationKind::Observed));
}

TEST(EtwInitializeJoin, MatchedExclusiveSkipsSharedEngine) {
    auto hit = ev(4242, 9000, "{0.0.1.00000000}.{cap}");
    hit.exclusive = true;
    hit.hresult = 0;
    const auto hint = matchEtwInitialize(chromeCap(), {hit}, 10000);
    const auto nodes = assemblePipeline(chromeCap(), {}, hint, {});
    EXPECT_NE(findNode(nodes, "engine"), nullptr);
    EXPECT_EQ(findNode(nodes, "sfx"), nullptr);
    EXPECT_EQ(findNode(nodes, "src"), nullptr);
}

TEST(EtwInitializeJoin, UnmatchedLeavesCategoryUnknown) {
    auto noise = ev(7, 9999, "{speakers}");
    noise.category = std::string("Media");
    noise.hresult = 0;
    const auto hint = matchEtwInitialize(chromeCap(), {noise}, 10000);
    const auto nodes = assemblePipeline(chromeCap(), {}, hint, {});
    const auto* mfx = findNode(nodes, "mfx");
    ASSERT_NE(mfx, nullptr);
    EXPECT_TRUE(hasParam(*mfx, "category", "unknown", ObservationKind::Unknown));
    const auto* session = findNode(nodes, "session");
    ASSERT_NE(session, nullptr);
    for (const auto& p : session->params) {
        EXPECT_NE(p.key, "Initialize HRESULT");
    }
}

TEST(EtwInitializeFields, DictionaryMapsPlaybackAndPerformance) {
    const std::vector<std::pair<std::string, std::string>> props = {
        {"PID", "4242"},
        {"Category", "3"},
        {"raw", "1"},
        {"matchformat", "0"},
        {"Endpoint", "{0.0.1.00000000}.{cap}"},
        {"HRESULT", "0"},
    };
    const auto f = etwFieldsFromProperties(0, 9000, props);
    EXPECT_EQ(f.processId, 4242u);
    EXPECT_EQ(f.timeMs, 9000);
    EXPECT_EQ(f.deviceId, "{0.0.1.00000000}.{cap}");
    ASSERT_TRUE(f.category.has_value());
    EXPECT_EQ(*f.category, "Communications");
    ASSERT_TRUE(f.raw.has_value());
    EXPECT_TRUE(*f.raw);
    ASSERT_TRUE(f.matchFormat.has_value());
    EXPECT_FALSE(*f.matchFormat);
    ASSERT_TRUE(f.hresult.has_value());
    EXPECT_EQ(*f.hresult, 0);
}

TEST(EtwInitializeFields, ShareModeMarksExclusive) {
    const auto f = etwFieldsFromProperties(11, 1, {{"ShareMode", "1"}});
    EXPECT_EQ(f.processId, 11u);
    ASSERT_TRUE(f.exclusive.has_value());
    EXPECT_TRUE(*f.exclusive);
}

TEST(EtwWatchStatus, EnableFailureTextIsUnavailable) {
    EXPECT_STREQ(etwWatchStatusText(EtwWatchStatus::Unavailable), "ETW unavailable");
    EXPECT_STREQ(etwWatchStatusText(EtwWatchStatus::Listening), "ETW listening");
}
