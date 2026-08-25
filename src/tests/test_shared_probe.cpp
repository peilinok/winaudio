#include <gtest/gtest.h>
#include "SharedProbe.h"

using namespace wa;

namespace {

class RecordingHost : public SharedProbeHost {
public:
    int opens = 0;
    int reads = 0;
    int closes = 0;
    std::vector<StreamParams> opened;
    Result openResult = Result::Ok();
    std::vector<AdvertisedEffect> effects = {{"Deep Noise Suppression", true, true}};

    Result open(const StreamParams& params) override {
        ++opens;
        opened.push_back(params);
        return openResult;
    }
    Result readEffects(std::vector<AdvertisedEffect>& out) override {
        ++reads;
        out = effects;
        return Result::Ok();
    }
    void close() override { ++closes; }
};

}  // namespace

TEST(SharedProbe, RecipesAreDefaultCommunicationsAndRaw) {
    const auto recipes = sharedProbeRecipes();
    ASSERT_EQ(recipes.size(), 3u);
    EXPECT_EQ(recipes[0].label, "Default");
    EXPECT_FALSE(recipes[0].raw);
    EXPECT_TRUE(recipes[0].params.isDefault());

    EXPECT_EQ(recipes[1].label, "Communications");
    EXPECT_FALSE(recipes[1].raw);
    EXPECT_TRUE(recipes[1].params.clientProperties.enabled);
    EXPECT_EQ(recipes[1].params.clientProperties.category, AudioCategory::Communications);
    EXPECT_EQ(recipes[1].params.clientProperties.option, StreamOption::None);

    EXPECT_EQ(recipes[2].label, "Raw");
    EXPECT_TRUE(recipes[2].raw);
    EXPECT_TRUE(recipes[2].params.clientProperties.enabled);
    EXPECT_EQ(recipes[2].params.clientProperties.option, StreamOption::Raw);
}

TEST(SharedProbe, ExclusiveFailsWithoutOpeningHost) {
    RecordingHost host;
    std::vector<ProbeSlice> slices = {{"stale", false, {}}};
    const Result r = runSharedProbes(host, true, slices);
    EXPECT_FALSE(r);
    EXPECT_NE(r.message.find("exclusive"), std::string::npos);
    EXPECT_EQ(host.opens, 0);
    EXPECT_EQ(host.reads, 0);
    EXPECT_EQ(host.closes, 0);
    ASSERT_EQ(slices.size(), 1u);
    EXPECT_EQ(slices[0].label, "stale");
}

TEST(SharedProbe, SharedRunsAllRecipesAndMarksProbedOnJoin) {
    RecordingHost host;
    std::vector<ProbeSlice> slices;
    const Result r = runSharedProbes(host, false, slices);
    EXPECT_TRUE(r);
    EXPECT_EQ(host.opens, 3);
    EXPECT_EQ(host.reads, 3);
    EXPECT_EQ(host.closes, 3);
    ASSERT_EQ(slices.size(), 3u);
    EXPECT_EQ(slices[0].label, "Default");
    EXPECT_EQ(slices[2].label, "Raw");
    EXPECT_TRUE(slices[2].raw);

    LiveSessionView session;
    session.processId = 1;
    session.processName = "chrome.exe";
    session.flow = PipelineFlow::Capture;
    EndpointSnapshot ep;
    const auto nodes = assemblePipeline(session, ep, {}, slices);
    bool sawProbed = false;
    for (const auto& n : nodes) {
        for (const auto& p : n.params) {
            if (p.key == "Default" || p.key == "Communications" || p.key == "Raw") {
                EXPECT_EQ(p.kind, ObservationKind::Probed);
                EXPECT_NE(p.kind, ObservationKind::Observed);
                sawProbed = true;
            }
        }
    }
    EXPECT_TRUE(sawProbed);
}

TEST(SharedProbe, OpenFailureStillClosesAndKeepsOtherRecipes) {
    RecordingHost host;
    host.openResult = Result::Fail(-1, "device in exclusive");
    std::vector<ProbeSlice> slices;
    const Result r = runSharedProbes(host, false, slices);
    EXPECT_TRUE(r);
    EXPECT_EQ(host.opens, 3);
    EXPECT_EQ(host.reads, 0);
    EXPECT_EQ(host.closes, 3);
    ASSERT_EQ(slices.size(), 3u);
    EXPECT_TRUE(slices[0].effects.empty());
    EXPECT_EQ(slices[0].error, "device in exclusive");

    LiveSessionView session;
    session.processId = 1;
    session.processName = "chrome.exe";
    session.flow = PipelineFlow::Capture;
    const auto nodes = assemblePipeline(session, {}, {}, slices);
    bool sawUnknown = false;
    for (const auto& n : nodes) {
        for (const auto& p : n.params) {
            if (p.key == "Default") {
                EXPECT_EQ(p.kind, ObservationKind::Unknown);
                EXPECT_EQ(p.value, "device in exclusive");
                sawUnknown = true;
            }
        }
    }
    EXPECT_TRUE(sawUnknown);
}
