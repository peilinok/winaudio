#include <gtest/gtest.h>
#include <string>
#include "AppUiText.h"
#include "EtwInitialize.h"
#include "OnDemandAttach.h"
#include "PipelineGraph.h"

using namespace wa;

namespace {

LiveSessionView row(uint32_t pid, const char* name, const char* device, PipelineFlow flow,
                   const char* instanceId = "") {
    LiveSessionView s;
    s.processId = pid;
    s.processName = name;
    s.deviceId = device;
    s.deviceName = device;
    s.flow = flow;
    s.sessionInstanceId = instanceId;
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

TEST(LiveSessionList, HideSelfIgnoresInstanceId) {
    std::vector<LiveSessionView> rows = {
        row(42, "WinAudioGui.exe", "speakers", PipelineFlow::Render, "self"),
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "other"),
    };
    shapeLiveSessionList(rows, 42);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].sessionInstanceId, "other");
    LiveSessionView self = row(42, "WinAudioGui.exe", "speakers", PipelineFlow::Render, "self");
    EXPECT_EQ(restoreLiveSessionSelection(rows, self), -1);
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

TEST(LiveSessionList, KeepsTwoRowsWhenInstanceIdsDiffer) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a"),
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-b"),
    };
    shapeLiveSessionList(rows, 42);
    ASSERT_EQ(rows.size(), 2u);

    LiveSessionView selected = row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-b");
    const int idx = restoreLiveSessionSelection(rows, selected);
    ASSERT_GE(idx, 0);
    EXPECT_EQ(rows[(size_t)idx].sessionInstanceId, "sid-b");
    EXPECT_EQ(rows[(size_t)idx].processId, 10u);
}

TEST(LiveSessionList, RestoreMissesWhenInstanceIdIsGone) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a"),
    };
    shapeLiveSessionList(rows, 0);
    LiveSessionView selected = row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-gone");
    EXPECT_EQ(restoreLiveSessionSelection(rows, selected), -1);
}

TEST(LiveSessionList, RestoreFallsBackWhenInstanceIdEmpty) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "mic", PipelineFlow::Capture, "sid-a"),
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-speakers"),
        row(11, "zoom.exe", "speakers", PipelineFlow::Render, "sid-zoom"),
    };
    shapeLiveSessionList(rows, 0);
    LiveSessionView selected = row(10, "chrome.exe", "speakers", PipelineFlow::Render);
    EXPECT_TRUE(selected.sessionInstanceId.empty());
    const int idx = restoreLiveSessionSelection(rows, selected);
    ASSERT_GE(idx, 0);
    EXPECT_EQ(rows[(size_t)idx].sessionInstanceId, "sid-speakers");
    EXPECT_EQ(rows[(size_t)idx].processId, 10u);
    EXPECT_EQ(rows[(size_t)idx].deviceId, "speakers");
    EXPECT_EQ(rows[(size_t)idx].flow, PipelineFlow::Render);
}

TEST(LiveSessionList, SnapshotAppearKeepsSelectedRow) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a"),
    };
    shapeLiveSessionList(rows, 42);
    ASSERT_EQ(rows.size(), 1u);
    const LiveSessionView selected = rows[0];

    rows.push_back(row(42, "WinAudioGui.exe", "speakers", PipelineFlow::Render, "sid-self"));
    rows.push_back(row(11, "zoom.exe", "mic", PipelineFlow::Capture, "sid-z"));
    shapeLiveSessionList(rows, 42);
    ASSERT_EQ(rows.size(), 2u);
    const int idx = restoreLiveSessionSelection(rows, selected);
    ASSERT_GE(idx, 0);
    EXPECT_EQ(rows[(size_t)idx].sessionInstanceId, "sid-a");
    EXPECT_EQ(rows[(size_t)(1 - idx)].sessionInstanceId, "sid-z");
}

TEST(LiveSessionList, SnapshotDisappearDropsSelectedRow) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a"),
        row(11, "zoom.exe", "mic", PipelineFlow::Capture, "sid-z"),
    };
    shapeLiveSessionList(rows, 0);
    const LiveSessionView selected = row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a");

    rows = {row(11, "zoom.exe", "mic", PipelineFlow::Capture, "sid-z")};
    shapeLiveSessionList(rows, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].sessionInstanceId, "sid-z");
    EXPECT_EQ(restoreLiveSessionSelection(rows, selected), -1);
}

TEST(LiveSessionList, SnapshotDropsOneOfTwoSamePidSessions) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a"),
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-b"),
    };
    shapeLiveSessionList(rows, 0);
    ASSERT_EQ(rows.size(), 2u);
    const LiveSessionView selected = row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-b");

    rows = {row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a")};
    shapeLiveSessionList(rows, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].sessionInstanceId, "sid-a");
    EXPECT_EQ(restoreLiveSessionSelection(rows, selected), -1);
}

TEST(LiveSessionList, TooltipKeepsProcessAndDeviceWhenCellsClip) {
    LiveSessionView s = row(25060, "chrome.exe", "Headset Microphone (Realtek(R) Audio)",
                            PipelineFlow::Capture);
    s.sessionVolume = 0.75f;
    s.sessionMute = true;
    s.state = "Active";
    const std::string tip = wa::ui_text::formatLiveSessionTooltip(s);
    EXPECT_NE(tip.find("chrome.exe"), std::string::npos);
    EXPECT_NE(tip.find("25060"), std::string::npos);
    EXPECT_NE(tip.find("Headset Microphone (Realtek(R) Audio)"), std::string::npos);
    EXPECT_NE(tip.find(wa::ui_text::kPipelineFlowCapture), std::string::npos);
    EXPECT_NE(tip.find("0.75"), std::string::npos);
    EXPECT_NE(tip.find(wa::ui_text::kPipelineMuteYes), std::string::npos);
    EXPECT_NE(tip.find("Active"), std::string::npos);
}

TEST(LiveSessionList, CellPatchUpdatesOnlyTheMatchingRow) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a"),
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-b"),
    };
    rows[1].sessionMute = true;
    rows[1].state = "Inactive";

    LiveSessionCellPatch patch;
    patch.sessionInstanceId = "sid-b";
    patch.processId = 99;
    patch.deviceId = "other-device";
    patch.flow = PipelineFlow::Capture;
    patch.hasVolume = true;
    patch.volume = 0.25f;
    patch.hasMute = true;
    patch.mute = false;
    patch.hasState = true;
    patch.state = "Active";

    EXPECT_EQ(applyLiveSessionCellPatch(rows, patch), 1);
    EXPECT_FLOAT_EQ(rows[0].sessionVolume, 1.f);
    EXPECT_FALSE(rows[0].sessionMute);
    EXPECT_EQ(rows[0].state, "Active");
    EXPECT_EQ(rows[0].processName, "chrome.exe");
    EXPECT_FLOAT_EQ(rows[1].sessionVolume, 0.25f);
    EXPECT_FALSE(rows[1].sessionMute);
    EXPECT_EQ(rows[1].state, "Active");
    EXPECT_EQ(rows[1].sessionInstanceId, "sid-b");
    EXPECT_EQ(rows[1].processName, "chrome.exe");
}

TEST(LiveSessionList, CellPatchDropsUnknownInstanceWithoutFallback) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a"),
    };
    LiveSessionCellPatch patch;
    patch.sessionInstanceId = "sid-missing";
    patch.processId = 10;
    patch.deviceId = "speakers";
    patch.flow = PipelineFlow::Render;
    patch.hasVolume = true;
    patch.volume = 0.1f;

    EXPECT_EQ(applyLiveSessionCellPatch(rows, patch), -1);
    EXPECT_FLOAT_EQ(rows[0].sessionVolume, 1.f);
    EXPECT_FALSE(rows[0].sessionMute);
    EXPECT_EQ(rows[0].state, "Active");
}

TEST(LiveSessionList, CellPatchFallsBackToPidDeviceFlow) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "mic", PipelineFlow::Capture, "sid-mic"),
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-spk"),
    };
    LiveSessionCellPatch patch;
    patch.processId = 10;
    patch.deviceId = "speakers";
    patch.flow = PipelineFlow::Render;
    patch.hasMute = true;
    patch.mute = true;

    EXPECT_EQ(applyLiveSessionCellPatch(rows, patch), 1);
    EXPECT_FALSE(rows[0].sessionMute);
    EXPECT_FLOAT_EQ(rows[0].sessionVolume, 1.f);
    EXPECT_TRUE(rows[1].sessionMute);
    EXPECT_FLOAT_EQ(rows[1].sessionVolume, 1.f);
    EXPECT_EQ(rows[1].state, "Active");
}

TEST(LiveSessionList, CellPatchVolumeLeavesMuteAndState) {
    std::vector<LiveSessionView> rows = {
        row(10, "chrome.exe", "speakers", PipelineFlow::Render, "sid-a"),
    };
    rows[0].sessionMute = true;
    rows[0].state = "Inactive";
    LiveSessionCellPatch patch;
    patch.sessionInstanceId = "sid-a";
    patch.hasVolume = true;
    patch.volume = 0.5f;

    EXPECT_EQ(applyLiveSessionCellPatch(rows, patch), 0);
    EXPECT_FLOAT_EQ(rows[0].sessionVolume, 0.5f);
    EXPECT_TRUE(rows[0].sessionMute);
    EXPECT_EQ(rows[0].state, "Inactive");
    EXPECT_EQ(rows[0].processName, "chrome.exe");
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
    EXPECT_STREQ(wa::ui_text::pipelineCallLogEmptyText(false, false),
                 wa::ui_text::kPipelineCallLogEmpty);
    EXPECT_STREQ(wa::ui_text::pipelineCallLogEmptyText(false, true),
                 wa::ui_text::kPipelineCallLogEmpty);
    EXPECT_STREQ(wa::ui_text::pipelineCallLogEmptyText(true, false),
                 wa::ui_text::kPipelineCallLogWaiting);
    EXPECT_STREQ(wa::ui_text::pipelineCallLogEmptyText(true, true),
                 wa::ui_text::kPipelineCallLogWaitingPump);
    EXPECT_NE(std::string(wa::ui_text::kPipelineCallLogWaiting).find("Initialize"),
              std::string::npos);
    EXPECT_NE(std::string(wa::ui_text::kPipelineCallLogWaiting).find("Record pump metadata"),
              std::string::npos);
    EXPECT_NE(std::string(wa::ui_text::kPipelineCallLogWaitingPump).find("Core Audio COM"),
              std::string::npos);
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
