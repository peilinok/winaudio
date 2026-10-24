#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "CallLogStore.h"
#include "PipelineAttachUiModel.h"
#include "PipelineGraph.h"

using namespace wa;

TEST(PipelineAttachUiModel, CopyLiveSessionPidToBuffer) {
    std::vector<LiveSessionView> rows(2);
    rows[0].processId = 100u;
    rows[1].processId = 4242u;
    char buf[32] = "stale";
    EXPECT_TRUE(pipeline_attach_ui::copyLiveSessionPidToBuffer(rows, 1, buf, sizeof(buf)));
    EXPECT_STREQ(buf, "4242");
    EXPECT_FALSE(pipeline_attach_ui::copyLiveSessionPidToBuffer(rows, -1, buf, sizeof(buf)));
    EXPECT_FALSE(pipeline_attach_ui::copyLiveSessionPidToBuffer(rows, 2, buf, sizeof(buf)));
    EXPECT_FALSE(pipeline_attach_ui::copyLiveSessionPidToBuffer(rows, 0, nullptr, 8));
}

TEST(PipelineAttachUiModel, ParseAttachPidRejectsZero) {
    uint32_t pid = 99;
    EXPECT_TRUE(pipeline_attach_ui::parseAttachPid("25060", pid));
    EXPECT_EQ(pid, 25060u);
    EXPECT_FALSE(pipeline_attach_ui::parseAttachPid("0", pid));
    EXPECT_FALSE(pipeline_attach_ui::parseAttachPid("abc", pid));
    EXPECT_FALSE(pipeline_attach_ui::parseAttachPid("", pid));
}

TEST(PipelineAttachUiModel, CanAttachOnlyBlocksSamePid) {
    EXPECT_TRUE(pipeline_attach_ui::canAttach(4242u, false));
    EXPECT_FALSE(pipeline_attach_ui::canAttach(0u, false));
    EXPECT_FALSE(pipeline_attach_ui::canAttach(4242u, true));
    // Another PID may still attach while 4242 is hooked.
    EXPECT_TRUE(pipeline_attach_ui::canAttach(7u, false));
}

TEST(PipelineAttachUiModel, StatusForPidUsesAttachedSetAndFailMap) {
    std::unordered_set<uint32_t> attached{4242u};
    std::unordered_map<uint32_t, std::string> fails{{7u, "cross-bitness"}};
    EXPECT_EQ(pipeline_attach_ui::statusForPid(4242u, attached, fails),
              PipelineAttachUiState::Attached);
    EXPECT_EQ(pipeline_attach_ui::statusForPid(7u, attached, fails),
              PipelineAttachUiState::Failed);
    EXPECT_EQ(pipeline_attach_ui::statusForPid(9u, attached, fails),
              PipelineAttachUiState::Detached);
    EXPECT_EQ(pipeline_attach_ui::statusForPid(0u, attached, fails),
              PipelineAttachUiState::Detached);
}

TEST(PipelineAttachUiModel, HookStatusFollowsAttachedSet) {
    LiveSessionView row{};
    row.processId = 4242u;
    row.deviceId = "dev";
    row.flow = PipelineFlow::Render;
    row.sessionInstanceId = "inst-a";
    std::unordered_map<std::string, PipelineAttachUiState> map;
    map[liveSessionAttachKey(row)] = PipelineAttachUiState::Detached;
    std::unordered_set<uint32_t> attached{4242u};
    std::unordered_map<uint32_t, std::string> fails;

    EXPECT_EQ(pipeline_attach_ui::hookStatusForRow(row, attached, fails, map),
              PipelineAttachUiState::Attached);

    attached.clear();
    fails[4242u] = "no debug";
    EXPECT_EQ(pipeline_attach_ui::hookStatusForRow(row, attached, fails, map),
              PipelineAttachUiState::Failed);

    fails.clear();
    LiveSessionView other = row;
    other.processId = 7u;
    other.sessionInstanceId = "inst-b";
    attached = {4242u};
    EXPECT_EQ(pipeline_attach_ui::hookStatusForRow(other, attached, fails, map),
              PipelineAttachUiState::Detached);
}

TEST(PipelineAttachUiModel, SessionHintOnlyWhenSelectedPidMatches) {
    std::vector<LiveSessionView> rows(1);
    rows[0].processId = 4242u;
    rows[0].deviceId = "dev-1";
    rows[0].flow = PipelineFlow::Render;
    rows[0].sessionInstanceId = "inst-1";
    std::string deviceId;
    PipelineFlow flow = PipelineFlow::Capture;
    std::string key;
    EXPECT_TRUE(pipeline_attach_ui::sessionHintForPid(rows, 0, 4242u, deviceId, flow, key));
    EXPECT_EQ(deviceId, "dev-1");
    EXPECT_EQ(flow, PipelineFlow::Render);
    EXPECT_EQ(key, "inst-1");
    EXPECT_FALSE(pipeline_attach_ui::sessionHintForPid(rows, 0, 7u, deviceId, flow, key));
    EXPECT_FALSE(pipeline_attach_ui::sessionHintForPid(rows, -1, 4242u, deviceId, flow, key));
}

TEST(PipelineAttachUiModel, FormatAttachedPidsAndDetachGates) {
    EXPECT_EQ(pipeline_attach_ui::formatAttachedPids({}), "");
    EXPECT_EQ(pipeline_attach_ui::formatAttachedPids({3u, 10u}), "3, 10");
    std::unordered_set<uint32_t> attached{3u};
    std::unordered_map<uint32_t, std::string> fails;
    EXPECT_TRUE(pipeline_attach_ui::canDetachPid(3u, attached, fails));
    EXPECT_FALSE(pipeline_attach_ui::canDetachPid(9u, attached, fails));
    EXPECT_TRUE(pipeline_attach_ui::canDetachAll(attached, fails));
    attached.clear();
    EXPECT_FALSE(pipeline_attach_ui::canDetachAll(attached, fails));
    fails[9u] = "fail";
    EXPECT_TRUE(pipeline_attach_ui::canDetachPid(9u, attached, fails));
    EXPECT_TRUE(pipeline_attach_ui::canDetachAll(attached, fails));
}

TEST(PipelineAttachUiModel, ReplacementKeepsFixedRequestAndCommitsOnlyOnSuccess) {
    using namespace pipeline_attach_ui;
    CallLogStore store;
    store.append({HookedCall{}});
    store.setRecording(true);
    const auto oldRound = store.round();
    RoundTransition transition;
    AttachRequest request{7, "device-a", PipelineFlow::Render, "session-a", true};
    EXPECT_EQ(transition.request(request, true, false), RoundAttachAction::Confirm);
    transition.saveThenContinue();
    transition.saved(false);
    EXPECT_FALSE(transition.ready());
    EXPECT_EQ(store.round(), oldRound);
    EXPECT_EQ(transition.request(request, true, true), RoundAttachAction::Busy);
    EXPECT_FALSE(transition.ready());
    EXPECT_EQ(transition.request(request, true, false), RoundAttachAction::Confirm);
    request.pid = 9;
    request.deviceId = "new-focus";
    transition.discard();
    ASSERT_TRUE(transition.ready());
    const auto fixed = transition.take();
    ASSERT_TRUE(fixed.has_value());
    EXPECT_EQ(fixed->pid, 7u);
    EXPECT_EQ(fixed->deviceId, "device-a");
    EXPECT_EQ(commitAttach(store, *fixed, false), 0u);
    EXPECT_EQ(store.round(), oldRound);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_NE(commitAttach(store, *fixed, true), 0u);
    EXPECT_NE(store.round(), oldRound);
    EXPECT_EQ(store.size(), 0u);
    EXPECT_TRUE(store.recording());
    EXPECT_TRUE(store.freezeExport().records.empty());
}

TEST(PipelineAttachUiModel, CancelAndSavedAttemptFailureKeepRangeAndDoNotQueueBusyRequest) {
    using namespace pipeline_attach_ui;
    CallLogStore store;
    store.append({HookedCall{}});
    store.setRecording(true);
    store.setRecording(false);
    const auto before = store.freezeExport();
    RoundTransition transition;
    const AttachRequest request{7, "device", PipelineFlow::Capture, "session", true};
    EXPECT_EQ(transition.request(request, true, true), RoundAttachAction::Busy);
    EXPECT_FALSE(transition.pending());
    EXPECT_FALSE(transition.take().has_value());
    EXPECT_EQ(transition.request(request, true, false), RoundAttachAction::Confirm);
    transition.cancel();
    EXPECT_FALSE(transition.take().has_value());
    EXPECT_EQ(transition.request(request, true, false), RoundAttachAction::Confirm);
    transition.saveThenContinue();
    transition.saved(true);
    const auto attempt = transition.take();
    ASSERT_TRUE(attempt.has_value());
    EXPECT_EQ(commitAttach(store, *attempt, false), 0u);
    EXPECT_EQ(store.freezeExport().records.front(), before.records.front());
    EXPECT_EQ(store.round(), before.round);
    EXPECT_EQ(store.exportCount(), 1u);
    // Same-round additional targets remain allowed during an export.
    auto sameRound = request;
    sameRound.replaceRound = false;
    EXPECT_EQ(transition.request(sameRound, true, true), RoundAttachAction::Attempt);
}
