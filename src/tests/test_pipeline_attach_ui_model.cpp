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
    EXPECT_EQ(store.round(), oldRound);
    EXPECT_EQ(store.size(), 1u);
    EXPECT_TRUE(store.recording());
    EXPECT_EQ(store.freezeExport().records.size(), 1u);
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

namespace {
PipelineAttachController::Event segmentEvent(ProcessInstance instance, uint64_t token, bool ended = false) {
    PipelineAttachController::Event event;
    event.pid = instance.pid;
    event.instance = instance;
    event.token = token;
    event.name = "target.exe";
    event.snapshotMs = ended ? 2500 : 1000;
    event.kind = ended ? PipelineAttachController::EventKind::Detached : PipelineAttachController::EventKind::Attached;
    event.result = Result::Ok();
    return event;
}
HookedCall segmentCall(ProcessInstance instance, uint64_t token, const char* argument) {
    HookedCall call;
    call.pid = instance.pid;
    call.processCreated = instance.created;
    call.attachToken = token;
    call.iface = "ISimpleAudioVolume";
    call.method = "SetMasterVolume";
    call.args = argument;
    call.hresult = 0;
    return call;
}
}

TEST(PipelineAttachUiModel, IndependentSegmentSelectionKeepsBackgroundReceptionAndHistory) {
    using namespace pipeline_attach_ui;
    const ProcessInstance a{7, 100}, b{9, 200};
    CallLogStore store;
    SegmentSelection selection;
    PipelineAttachController::Batch batch;
    batch.events = {segmentEvent(a, 11), segmentEvent(b, 22)};
    selection.apply(store, std::move(batch));
    const auto first = store.attachForToken(11), second = store.attachForToken(22);
    EXPECT_EQ(selection.selected(), first);
    // No Live session events are required to keep recording either process.
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(selection.select(store, i % 2 ? first : second));
        PipelineAttachController::Batch received;
        received.calls = {segmentCall(a, 11, "level=0.250"), segmentCall(b, 22, "level=0.750")};
        selection.apply(store, std::move(received));
        call_log_filter::FilterOpts query;
        query.attach = selection.selected();
        const auto view = store.view(query);
        ASSERT_EQ(view.size(), static_cast<size_t>(i + 1));
        const auto* row = view.at(static_cast<size_t>(i));
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->call.pid, i % 2 ? 7u : 9u);
        EXPECT_EQ(row->call.args, i % 2 ? "level=0.250" : "level=0.750");
    }
    EXPECT_EQ(store.segment(first)->count, 100u);
    EXPECT_EQ(store.segment(second)->count, 100u);
    PipelineAttachController::Batch ended;
    auto exit = segmentEvent(a, 11, true);
    exit.exited = true;
    ended.events = {exit};
    ended.calls = {segmentCall(a, 11, "level=0.500")};
    selection.apply(store, std::move(ended));
    EXPECT_EQ(selection.selected(), first);
    ASSERT_EQ(store.segments(false).size(), 1u);
    EXPECT_EQ(store.segment(first)->state, CallLogStore::SegmentState::Exited);
    EXPECT_EQ(store.segment(first)->snapshotMs, 2500);
    EXPECT_TRUE(store.segment(first)->coverageUnknown);
    EXPECT_EQ(store.segment(first)->count, 101u);
    EXPECT_EQ(store.segments(true).front().id, second);
    PipelineAttachController::Batch lateEnd;
    auto duplicate = exit;
    duplicate.snapshotMs = 9999;
    lateEnd.events = {duplicate};
    selection.apply(store, std::move(lateEnd));
    EXPECT_EQ(store.segment(first)->snapshotMs, 2500);
}

TEST(PipelineAttachUiModel, ReattachRetainsHistoryRejectsLateTokensAndSeparatesReusedPid) {
    using namespace pipeline_attach_ui;
    const ProcessInstance old{7, 100}, reused{7, 300};
    CallLogStore store;
    SegmentSelection selection;
    PipelineAttachController::Batch first;
    first.events = {segmentEvent(old, 11)};
    first.calls = {segmentCall(old, 11, "level=0.250")};
    selection.apply(store, std::move(first));
    const auto historical = selection.selected();
    const auto kept = store.view().at(0)->id;
    PipelineAttachController::Batch end;
    end.events = {segmentEvent(old, 11, true)};
    selection.apply(store, std::move(end));
    const auto round = store.round();
    PipelineAttachController::Batch reattach;
    reattach.events = {segmentEvent(old, 22)};
    reattach.calls = {segmentCall(old, 11, "late"), segmentCall(old, 22, "level=0.750")};
    selection.apply(store, std::move(reattach));
    const auto fresh = store.attachForToken(22);
    EXPECT_NE(fresh, historical);
    EXPECT_EQ(selection.selected(), historical);
    EXPECT_EQ(store.round(), round);
    EXPECT_EQ(store.size(), 2u);
    EXPECT_NE(store.find(kept), nullptr);
    // Repeated active instance success is idempotent, not a new segment.
    EXPECT_EQ(store.registerAttach(old, "target.exe", 33, 1200), fresh);
    PipelineAttachController::Batch replaced;
    replaced.events = {segmentEvent(old, 22, true), segmentEvent(reused, 44)};
    replaced.calls = {segmentCall(old, 22, "last-before-exit")};
    selection.apply(store, std::move(replaced));
    EXPECT_EQ(store.segment(fresh)->snapshotMs, 2500);
    EXPECT_EQ(store.segment(fresh)->count, 2u);
    PipelineAttachController::Batch late;
    late.calls = {segmentCall(old, 22, "late-old"), segmentCall(old, 44, "wrong-instance"),
                  segmentCall(reused, 44, "level=1.000")};
    selection.apply(store, std::move(late));
    EXPECT_EQ(store.size(), 4u);
    EXPECT_EQ(store.latestAttach(old), fresh);
    EXPECT_EQ(store.latestAttach(reused), store.attachForToken(44));
    EXPECT_EQ(store.segments(false).size(), 2u);
    EXPECT_EQ(selection.selected(), historical);
    call_log_filter::FilterOpts query;
    query.attach = historical;
    EXPECT_EQ(store.view(query).position(kept), 0u);
    query.attach = store.attachForToken(44);
    const auto current = store.view(query);
    ASSERT_EQ(current.size(), 1u);
    EXPECT_EQ(current.position(kept), CallLogStore::kNotFound);
    EXPECT_EQ(current.at(0)->call.args, "level=1.000");
}

TEST(PipelineAttachUiModel, FailedAttachKeepsTheSelectedHistoryAndDoesNotCreateSegment) {
    CallLogStore store;
    pipeline_attach_ui::SegmentSelection selection;
    PipelineAttachController::Batch original;
    original.events = {segmentEvent({7, 100}, 11)};
    original.calls = {segmentCall({7, 100}, 11, "level=0.250")};
    selection.apply(store, std::move(original));
    PipelineAttachController::Batch end;
    end.events = {segmentEvent({7, 100}, 11, true)};
    selection.apply(store, std::move(end));
    PipelineAttachController::Batch failed;
    auto event = segmentEvent({9, 200}, 0);
    event.kind = PipelineAttachController::EventKind::Failed;
    event.result = Result::Fail(static_cast<long>(0x80004005), "failed");
    failed.events = {event};
    selection.apply(store, std::move(failed));
    EXPECT_EQ(selection.selected(), store.attachForToken(11));
    EXPECT_EQ(store.segments(false).size(), 1u);
    EXPECT_TRUE(store.segments(true).empty());
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.view().at(0)->call.args, "level=0.250");
}
