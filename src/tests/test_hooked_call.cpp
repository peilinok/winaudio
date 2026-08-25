#include <gtest/gtest.h>
#include "HookedCall.h"
#include "PipelineGraph.h"

using namespace wa;

namespace {

LiveSessionView chromeCap() {
    LiveSessionView s;
    s.processId = 4242;
    s.processName = "chrome.exe";
    s.deviceId = "{cap}";
    s.deviceName = "Headset Mic";
    s.flow = PipelineFlow::Capture;
    return s;
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

HookedCall initCall() {
    HookedCall c;
    c.streamId = 7;
    c.timeMs = 1000;
    c.iface = "IAudioClient";
    c.method = "Initialize";
    c.args = "share=shared flags=0x0 fmt=48000/32f/2";
    c.hresult = 0;
    c.exclusive = false;
    c.format = std::string("48000/32f/2");
    return c;
}

}  // namespace

TEST(CallLog, DropsPumpWhenDisabled) {
    HookedCall pump;
    pump.iface = "IAudioCaptureClient";
    pump.method = "GetBuffer";
    pump.args = "frames=480";
    pump.pump = true;
    HookedCall init = initCall();
    const auto log = shapeCallLog({pump, init}, false);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].method, "Initialize");
}

TEST(CallLog, KeepsControlPathActivateAndInitialize) {
    HookedCall act;
    act.iface = "IMMDevice";
    act.method = "Activate";
    act.args = "iid=IAudioClient";
    act.hresult = 0;
    const auto log = shapeCallLog({act, initCall()}, false);
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0].method, "Activate");
    EXPECT_EQ(log[1].method, "Initialize");
}

TEST(CallLog, NeverLeavesPcmInArgs) {
    HookedCall c = initCall();
    c.args = "share=shared pcm=010203 frames=480";
    const auto log = shapeCallLog({c}, false);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].args.find("pcm="), std::string::npos);
    EXPECT_NE(log[0].args.find("share=shared"), std::string::npos);
}

TEST(HookedJoin, InitializeOverridesEtwCategoryAndRaw) {
    EtwInitializeHint etw;
    etw.present = true;
    etw.category = std::string("Media");
    etw.raw = false;
    etw.hresult = 1;

    HookedCall hooked = initCall();
    hooked.category = std::string("Communications");
    hooked.raw = true;
    hooked.hresult = 0;

    const auto nodes = assemblePipeline(chromeCap(), {}, etw, {}, {hooked});
    const auto* sfx = findNode(nodes, "sfx");
    ASSERT_NE(sfx, nullptr);
    EXPECT_EQ(sfx->kind, ObservationKind::Skipped);
    EXPECT_TRUE(hasParam(*sfx, "RAW", "SFX not used", ObservationKind::Observed));
    const auto* mfx = findNode(nodes, "mfx");
    ASSERT_NE(mfx, nullptr);
    EXPECT_TRUE(hasParam(*mfx, "category", "Communications", ObservationKind::Observed));
    const auto* session = findNode(nodes, "session");
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(hasParam(*session, "Initialize HRESULT", "0", ObservationKind::Observed));
    EXPECT_TRUE(hasParam(*session, "app stream format", "48000/32f/2", ObservationKind::Observed));
}

TEST(HookedJoin, ExclusiveHookedSkipsSharedEngine) {
    HookedCall hooked = initCall();
    hooked.exclusive = true;
    hooked.args = "share=exclusive";
    const auto nodes = assemblePipeline(chromeCap(), {}, {}, {}, {hooked});
    EXPECT_NE(findNode(nodes, "engine"), nullptr);
    EXPECT_EQ(findNode(nodes, "sfx"), nullptr);
    EXPECT_EQ(findNode(nodes, "src"), nullptr);
}

TEST(HookedJoin, WithoutHookedCallsEtwStillApplies) {
    EtwInitializeHint etw;
    etw.present = true;
    etw.category = std::string("Media");
    const auto nodes = assemblePipeline(chromeCap(), {}, etw, {}, {});
    const auto* mfx = findNode(nodes, "mfx");
    ASSERT_NE(mfx, nullptr);
    EXPECT_TRUE(hasParam(*mfx, "category", "Media", ObservationKind::Observed));
}

TEST(HookedJoin, PumpRecordsDoNotProduceInitializeHint) {
    HookedCall pump;
    pump.method = "GetBuffer";
    pump.pump = true;
    pump.raw = true;
    pump.hresult = 0;
    EtwInitializeHint etw;
    etw.present = true;
    etw.category = std::string("Media");
    const auto nodes = assemblePipeline(chromeCap(), {}, etw, {}, {pump});
    const auto* sfx = findNode(nodes, "sfx");
    ASSERT_NE(sfx, nullptr);
    EXPECT_NE(sfx->kind, ObservationKind::Skipped);
    const auto* mfx = findNode(nodes, "mfx");
    ASSERT_NE(mfx, nullptr);
    EXPECT_TRUE(hasParam(*mfx, "category", "Media", ObservationKind::Observed));
}
