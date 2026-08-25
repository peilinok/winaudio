#include <gtest/gtest.h>
#include "AppUiText.h"
#include "EtwInitialize.h"
#include "OnDemandAttach.h"
#include "PipelineGraph.h"

using namespace wa;

namespace {

LiveSessionView row(uint32_t pid, const char* name, const char* device, PipelineFlow flow) {
    LiveSessionView s;
    s.processId = pid;
    s.processName = name;
    s.deviceId = device;
    s.deviceName = device;
    s.flow = flow;
    s.sessionVolume = 1.f;
    s.state = "Active";
    return s;
}

}  // namespace

TEST(LiveSessionList, DropsPidZero) {
    std::vector<LiveSessionView> rows = {
        row(0, "system", "mic", PipelineFlow::Capture),
        row(10, "chrome.exe", "mic", PipelineFlow::Capture),
    };
    shapeLiveSessionList(rows, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].processId, 10u);
}

TEST(LiveSessionList, HidesSelfPid) {
    std::vector<LiveSessionView> rows = {
        row(42, "WinAudioGui.exe", "speakers", PipelineFlow::Render),
        row(10, "chrome.exe", "mic", PipelineFlow::Capture),
    };
    shapeLiveSessionList(rows, 42);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].processId, 10u);
}

TEST(LiveSessionList, HidePidZeroKeepsOthers) {
    std::vector<LiveSessionView> rows = {row(10, "chrome.exe", "mic", PipelineFlow::Capture)};
    shapeLiveSessionList(rows, 0);
    ASSERT_EQ(rows.size(), 1u);
}

TEST(LiveSessionList, KeepsSamePidOnDifferentDevices) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "headset", PipelineFlow::Render),
        row(10, "chrome.exe", "mic", PipelineFlow::Capture),
    };
    shapeLiveSessionList(rows, 0);
    ASSERT_EQ(rows.size(), 2u);
}

TEST(LiveSessionList, SortsByNameThenPidThenFlowThenDevice) {
    std::vector<LiveSessionView> rows = {
        row(20, "zoom.exe", "mic", PipelineFlow::Capture),
        row(10, "chrome.exe", "speakers", PipelineFlow::Render),
        row(10, "chrome.exe", "headset", PipelineFlow::Render),
        row(10, "chrome.exe", "mic", PipelineFlow::Capture),
    };
    shapeLiveSessionList(rows, 0);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].processName, "chrome.exe");
    EXPECT_EQ(rows[0].flow, PipelineFlow::Capture);
    EXPECT_EQ(rows[1].deviceName, "headset");
    EXPECT_EQ(rows[2].deviceName, "speakers");
    EXPECT_EQ(rows[3].processName, "zoom.exe");
}

TEST(PipelineUiText, ExposesPipelineTabControls) {
    EXPECT_STREQ(wa::ui_text::kPipelineTab, "Pipeline");
    EXPECT_STREQ(wa::ui_text::kPipelineRefresh, "Refresh");
    EXPECT_STREQ(wa::ui_text::kPipelineSessions, "Live sessions");
    EXPECT_STREQ(wa::ui_text::kPipelineShowSelf, "Show this process");
    EXPECT_STREQ(wa::ui_text::kPipelineEmpty,
                 "No Live sessions. Start capture or playback in another app, then Refresh.");
    EXPECT_STREQ(wa::ui_text::kPipelineSelectHint,
                 "Select a Live session to see its processing graph.");
    EXPECT_STREQ(wa::ui_text::kPipelineGraph, "Processing graph");
    EXPECT_STREQ(wa::ui_text::kPipelineProbe, "Probe this device");
    EXPECT_STREQ(wa::ui_text::kPipelineEtwUnavailable, "ETW unavailable");
    EXPECT_STREQ(wa::ui_text::kPipelineEtwListening, "ETW listening");
    EXPECT_STREQ(wa::etwWatchStatusText(wa::EtwWatchStatus::Unavailable),
                 wa::ui_text::kPipelineEtwUnavailable);
    EXPECT_STREQ(wa::etwWatchStatusText(wa::EtwWatchStatus::Listening),
                 wa::ui_text::kPipelineEtwListening);
    EXPECT_STREQ(wa::ui_text::kPipelineAttach, "Attach");
    EXPECT_STREQ(wa::ui_text::kPipelineCallLog, "Call log");
    EXPECT_STREQ(wa::ui_text::kPipelineAttached, "Attached");
    EXPECT_STREQ(wa::ui_text::kPipelineCallLogEmpty,
                 "No control-path calls yet. Attach to intercept Core Audio COM.");
    EXPECT_STREQ(wa::attachBlockText(wa::AttachBlock::CrossBitness),
                 wa::ui_text::kPipelineCrossBitness);
    EXPECT_STREQ(wa::attachBlockText(wa::AttachBlock::NoDebugRights),
                 wa::ui_text::kPipelineNoDebug);
    EXPECT_STREQ(wa::attachBlockText(wa::AttachBlock::None), wa::ui_text::kPipelineAttached);
    EXPECT_STREQ(wa::ui_text::kPipelineCallColIface, "Iface");
    EXPECT_STREQ(wa::ui_text::kPipelineCallColMethod, "Method");
    EXPECT_STREQ(wa::ui_text::kPipelineCallColArgs, "Args");
    EXPECT_STREQ(wa::ui_text::kPipelineCallColHr, "HR");
    EXPECT_STREQ(wa::ui_text::kPipelineCallColStream, "Stream");
    EXPECT_STREQ(wa::ui_text::kPipelinePump, "Record pump metadata");
    EXPECT_STREQ(wa::ui_text::kPipelineXruns, "xruns");
}
