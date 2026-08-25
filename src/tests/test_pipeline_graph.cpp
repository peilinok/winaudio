#include <gtest/gtest.h>
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
    s.sessionVolume = 0.75f;
    s.sessionMute = false;
    return s;
}

LiveSessionView chromeRen() {
    auto s = chromeCap();
    s.flow = PipelineFlow::Render;
    s.deviceId = "{ren}";
    s.deviceName = "Speakers";
    return s;
}

EndpointSnapshot endpointWithApos() {
    EndpointSnapshot e;
    e.mixFormat = "48000/32f/2";
    e.deviceFormat = "48000/24/2";
    e.apos = {
        {"SFX", "{sfx-guid}", "OEM EQ"},
        {"MFX", "{mfx-guid}", "OEM NS"},
        {"EFX", "{efx-guid}", "Speaker protect"},
    };
    e.hardware = {{"mute", "false"}, {"volume", "-6 dB"}};
    return e;
}

const PipelineNode* findNode(const std::vector<PipelineNode>& nodes, const char* id) {
    for (const auto& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

std::vector<std::string> ids(const std::vector<PipelineNode>& nodes) {
    std::vector<std::string> out;
    out.reserve(nodes.size());
    for (const auto& n : nodes) out.push_back(n.id);
    return out;
}

bool hasParam(const PipelineNode& n, const char* key, const char* value, ObservationKind kind) {
    for (const auto& p : n.params) {
        if (p.key == key && p.value == value && p.kind == kind) return true;
    }
    return false;
}

}  // namespace

TEST(ObservationKind, Names) {
    EXPECT_STREQ(observationKindName(ObservationKind::Observed), "Observed");
    EXPECT_STREQ(observationKindName(ObservationKind::Probed), "Probed");
    EXPECT_STREQ(observationKindName(ObservationKind::Inferred), "Inferred");
    EXPECT_STREQ(observationKindName(ObservationKind::Skipped), "Skipped");
    EXPECT_STREQ(observationKindName(ObservationKind::Unknown), "Unknown");
}

TEST(PipelineGraph, CaptureOrderHardwareToApp) {
    const auto nodes = assemblePipeline(chromeCap(), endpointWithApos(), {}, {});
    EXPECT_EQ(ids(nodes), (std::vector<std::string>{
        "hardware", "driver", "efx", "mfx", "sfx", "src", "session", "app"}));
}

TEST(PipelineGraph, RenderOrderAppToHardware) {
    const auto nodes = assemblePipeline(chromeRen(), endpointWithApos(), {}, {});
    EXPECT_EQ(ids(nodes), (std::vector<std::string>{
        "app", "session", "src", "sfx", "mfx", "efx", "driver", "hardware"}));
}

TEST(PipelineGraph, SessionFieldsAreObserved) {
    const auto nodes = assemblePipeline(chromeCap(), endpointWithApos(), {}, {});
    const auto* session = findNode(nodes, "session");
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->kind, ObservationKind::Observed);
    EXPECT_TRUE(hasParam(*session, "pid", "4242", ObservationKind::Observed));
    EXPECT_TRUE(hasParam(*session, "mute", "false", ObservationKind::Observed));
    const auto* app = findNode(nodes, "app");
    ASSERT_NE(app, nullptr);
    EXPECT_TRUE(hasParam(*app, "flow", "capture", ObservationKind::Observed));
}

TEST(PipelineGraph, MixFormatIsNotClaimedAsAppFormat) {
    const auto nodes = assemblePipeline(chromeCap(), endpointWithApos(), {}, {});
    const auto* src = findNode(nodes, "src");
    ASSERT_NE(src, nullptr);
    EXPECT_TRUE(hasParam(*src, "mix format", "48000/32f/2", ObservationKind::Observed));
    EXPECT_TRUE(hasParam(*src, "note", "mix format is the engine, not the app stream format",
                         ObservationKind::Inferred));
}

TEST(PipelineGraph, RegisteredAposAreObservedOnSlots) {
    const auto nodes = assemblePipeline(chromeCap(), endpointWithApos(), {}, {});
    const auto* sfx = findNode(nodes, "sfx");
    ASSERT_NE(sfx, nullptr);
    EXPECT_TRUE(hasParam(*sfx, "SFX", "OEM EQ ({sfx-guid})", ObservationKind::Observed));
    const auto* efx = findNode(nodes, "efx");
    ASSERT_NE(efx, nullptr);
    EXPECT_TRUE(hasParam(*efx, "EFX", "Speaker protect ({efx-guid})", ObservationKind::Observed));
}

TEST(PipelineGraph, SysFxOffSkipsSfxAndMfx) {
    auto ep = endpointWithApos();
    ep.sysFxDisabled = true;
    const auto nodes = assemblePipeline(chromeCap(), ep, {}, {});
    const auto* sfx = findNode(nodes, "sfx");
    const auto* mfx = findNode(nodes, "mfx");
    ASSERT_NE(sfx, nullptr);
    ASSERT_NE(mfx, nullptr);
    EXPECT_EQ(sfx->kind, ObservationKind::Skipped);
    EXPECT_EQ(mfx->kind, ObservationKind::Skipped);
    const auto* efx = findNode(nodes, "efx");
    ASSERT_NE(efx, nullptr);
    EXPECT_EQ(efx->kind, ObservationKind::Observed);
}

TEST(PipelineGraph, RawEtwSkipsSfx) {
    EtwInitializeHint etw;
    etw.present = true;
    etw.raw = true;
    etw.category = std::string("Communications");
    etw.hresult = 0;
    const auto nodes = assemblePipeline(chromeCap(), endpointWithApos(), etw, {});
    const auto* sfx = findNode(nodes, "sfx");
    ASSERT_NE(sfx, nullptr);
    EXPECT_EQ(sfx->kind, ObservationKind::Skipped);
    EXPECT_TRUE(hasParam(*sfx, "RAW", "SFX not used", ObservationKind::Observed));
    const auto* mfx = findNode(nodes, "mfx");
    ASSERT_NE(mfx, nullptr);
    EXPECT_TRUE(hasParam(*mfx, "category", "Communications", ObservationKind::Observed));
}

TEST(PipelineGraph, ExclusiveSkipsEngine) {
    EtwInitializeHint etw;
    etw.present = true;
    etw.exclusive = true;
    etw.hresult = 0;
    const auto nodes = assemblePipeline(chromeRen(), endpointWithApos(), etw, {});
    EXPECT_NE(findNode(nodes, "engine"), nullptr);
    EXPECT_EQ(findNode(nodes, "sfx"), nullptr);
    EXPECT_EQ(findNode(nodes, "mfx"), nullptr);
    EXPECT_EQ(findNode(nodes, "src"), nullptr);
    const auto* engine = findNode(nodes, "engine");
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->kind, ObservationKind::Skipped);
    const auto* efx = findNode(nodes, "efx");
    ASSERT_NE(efx, nullptr);
    EXPECT_EQ(efx->kind, ObservationKind::Unknown);
}

TEST(PipelineGraph, WithoutEtwCategoryStaysUnknownOnMfx) {
    const auto nodes = assemblePipeline(chromeCap(), endpointWithApos(), {}, {});
    const auto* mfx = findNode(nodes, "mfx");
    ASSERT_NE(mfx, nullptr);
    EXPECT_TRUE(hasParam(*mfx, "category", "unknown", ObservationKind::Unknown));
}

TEST(PipelineGraph, ProbeSlicesAreProbedNotObserved) {
    ProbeSlice comm;
    comm.label = "Communications";
    comm.effects = {{"Deep Noise Suppression", true, true}};
    const auto nodes = assemblePipeline(chromeCap(), endpointWithApos(), {}, {comm});
    const auto* sfx = findNode(nodes, "sfx");
    ASSERT_NE(sfx, nullptr);
    EXPECT_TRUE(hasParam(*sfx, "Communications", "Deep Noise Suppression ON (settable)",
                         ObservationKind::Probed));
    for (const auto& p : sfx->params) {
        if (p.key == "Communications") {
            EXPECT_NE(p.kind, ObservationKind::Observed);
        }
    }
}

TEST(PipelineGraph, UnmatchedEtwAbsentDoesNotInventHresult) {
    const auto nodes = assemblePipeline(chromeCap(), endpointWithApos(), {}, {});
    const auto* session = findNode(nodes, "session");
    ASSERT_NE(session, nullptr);
    for (const auto& p : session->params) {
        EXPECT_NE(p.key, "Initialize HRESULT");
    }
}
