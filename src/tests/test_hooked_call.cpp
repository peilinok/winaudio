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
    ASSERT_EQ(log.entries.size(), 1u);
    EXPECT_EQ(log.entries[0].method, "Initialize");
    EXPECT_EQ(log.pumpXruns, 0u);
}

TEST(CallLog, KeepsControlPathActivateAndInitialize) {
    HookedCall act;
    act.iface = "IMMDevice";
    act.method = "Activate";
    act.args = "iid=IAudioClient";
    act.hresult = 0;
    const auto log = shapeCallLog({act, initCall()}, false);
    ASSERT_EQ(log.entries.size(), 2u);
    EXPECT_EQ(log.entries[0].method, "Activate");
    EXPECT_EQ(log.entries[1].method, "Initialize");
}

TEST(CallLog, NeverLeavesPcmInArgs) {
    HookedCall c = initCall();
    c.args = "share=shared pcm=010203 frames=480";
    const auto log = shapeCallLog({c}, false);
    ASSERT_EQ(log.entries.size(), 1u);
    EXPECT_EQ(log.entries[0].args.find("pcm="), std::string::npos);
    EXPECT_NE(log.entries[0].args.find("share=shared"), std::string::npos);
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

TEST(CallLog, PumpRingDropsOldestAndKeepsControlPath) {
    std::vector<HookedCall> in;
    HookedCall init = initCall();
    init.timeMs = 1;
    in.push_back(init);
    for (int i = 0; i < 20; ++i) {
        HookedCall p;
        p.iface = "IAudioCaptureClient";
        p.method = "GetBuffer";
        p.args = "frames=480";
        p.pump = true;
        p.timeMs = 10 + i;
        p.hresult = 0;
        in.push_back(p);
    }
    HookedCall stop;
    stop.iface = "IAudioClient";
    stop.method = "Stop";
    stop.timeMs = 1000;
    in.push_back(stop);

    const auto log = shapeCallLog(in, true, 8);
    size_t control = 0, pump = 0;
    bool sawInit = false, sawStop = false;
    int firstPumpTime = -1, lastPumpTime = -1;
    for (const auto& c : log.entries) {
        if (c.pump || isPumpMethod(c.method)) {
            ++pump;
            if (firstPumpTime < 0) firstPumpTime = static_cast<int>(c.timeMs);
            lastPumpTime = static_cast<int>(c.timeMs);
        } else {
            ++control;
            if (c.method == "Initialize") sawInit = true;
            if (c.method == "Stop") sawStop = true;
        }
    }
    EXPECT_EQ(control, 2u);
    EXPECT_TRUE(sawInit);
    EXPECT_TRUE(sawStop);
    EXPECT_EQ(pump, 8u);
    EXPECT_EQ(firstPumpTime, 22);
    EXPECT_EQ(lastPumpTime, 29);
}

TEST(CallLog, XrunAggregationCountsDroppedPumpRecords) {
    std::vector<HookedCall> in;
    in.push_back(initCall());
    for (int i = 0; i < 12; ++i) {
        HookedCall p;
        p.method = "GetBuffer";
        p.pump = true;
        p.xrun = (i % 3 == 0);
        p.hresult = 0;
        p.args = "frames=480";
        in.push_back(p);
    }
    const auto log = shapeCallLog(in, true, 4);
    EXPECT_EQ(log.pumpXruns, 4u);
    size_t pump = 0;
    for (const auto& c : log.entries)
        if (c.pump) ++pump;
    EXPECT_EQ(pump, 4u);
}

TEST(CallLog, EnabledPumpStillStripsPcm) {
    HookedCall p;
    p.method = "GetBuffer";
    p.pump = true;
    p.args = "frames=480 pcm=DEADBEEF";
    const auto log = shapeCallLog({initCall(), p}, true, 8);
    ASSERT_EQ(log.entries.size(), 2u);
    for (const auto& c : log.entries) {
        EXPECT_EQ(c.args.find("pcm="), std::string::npos);
    }
}
