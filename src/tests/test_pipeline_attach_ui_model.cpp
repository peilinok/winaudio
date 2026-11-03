#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <thread>
#include "CallLogStore.h"
#include "PipelineAttachUiModel.h"
#include "PipelineGraph.h"

using namespace wa;

TEST(PipelineAttachUiModel, BatchCheckboxesShareAnInstanceAndFreezeVisibleOrderWithoutRetargeting) {
    pipeline_attach_ui::BatchSelection selected;
    selected.set({20, "render", PipelineFlow::Render, "a", {20, 200}}, true);
    selected.set({10, "capture", PipelineFlow::Capture, "b", {10, 100}}, true);
    EXPECT_TRUE(selected.contains(20, 200));
    EXPECT_FALSE(selected.contains(20, 201));
    const auto frozen = selected.freeze({10, 20, 20, 30});
    ASSERT_EQ(frozen.size(), 2u);
    EXPECT_EQ(frozen[0].instance, (ProcessInstance{10, 100}));
    EXPECT_EQ(frozen[1].instance, (ProcessInstance{20, 200}));
    selected.set({20}, false);
    EXPECT_FALSE(selected.contains(20));
    EXPECT_EQ(frozen[1].deviceId, "render");
    EXPECT_EQ(frozen[1].flow, PipelineFlow::Render);
    EXPECT_EQ(selected.freeze({20, 10}).size(), 1u);
}

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

TEST(PipelineAttachUiModel, MethodPublicationKeepsDraftFailureHistoryAndInstancesSeparate) {
    CallLogStore store;
    pipeline_attach_ui::SegmentSelection selection;
    PipelineAttachController::Batch attached;
    auto first = segmentEvent({7, 100}, 11);
    first.methods = methodBit(MonitorMethod::SetMute);
    attached.events = {first, segmentEvent({9, 200}, 22)};
    selection.apply(store, std::move(attached));
    const auto old = store.attachForToken(11), background = store.attachForToken(22);
    pipeline_attach_ui::MethodDraft draft{methodBit(MonitorMethod::GetChannelCount)};
    EXPECT_TRUE(draft.dirty(store.segment(old)->methods));
    EXPECT_EQ(store.segment(old)->methods, methodBit(MonitorMethod::SetMute));
    PipelineAttachController::Batch failed;
    auto publication = first;
    publication.kind = PipelineAttachController::EventKind::ConfigurationFailed;
    publication.methods = draft.methods;
    publication.result = Result::Fail(static_cast<long>(0x80004005), "publish failed");
    failed.events = {publication};
    selection.apply(store, std::move(failed));
    EXPECT_EQ(store.segment(old)->methods, methodBit(MonitorMethod::SetMute));
    publication.kind = PipelineAttachController::EventKind::Configured;
    publication.result = Result::Ok();
    PipelineAttachController::Batch success;
    success.events = {publication};
    selection.apply(store, std::move(success));
    EXPECT_FALSE(draft.dirty(store.segment(old)->methods));
    EXPECT_EQ(store.segment(background)->methods, kDefaultMonitorMethods);
    PipelineAttachController::Batch reattached;
    reattached.events = {segmentEvent({7, 100}, 11, true), segmentEvent({7, 300}, 33)};
    selection.apply(store, std::move(reattached));
    const auto current = store.attachForToken(33);
    EXPECT_EQ(store.segment(current)->methods, kDefaultMonitorMethods);
    EXPECT_FALSE(store.setMethods(old, kAllMonitorMethods));
    PipelineAttachController::Batch stale;
    stale.events = {publication};
    selection.apply(store, std::move(stale));
    EXPECT_EQ(store.segment(old)->methods, draft.methods);
    EXPECT_EQ(store.segment(current)->methods, kDefaultMonitorMethods);
    EXPECT_EQ(selection.selected(), old);
}

TEST(PipelineAttachUiModel, TransportSnapshotBelongsToItsSegmentAndFreezesAtEnd) {
    CallLogStore store;
    pipeline_attach_ui::SegmentSelection selection;
    const ProcessInstance a{7, 100}, b{9, 200};
    PipelineAttachController::Batch initial;
    initial.events = {segmentEvent(a, 11), segmentEvent(b, 22)};
    selection.apply(store, std::move(initial));
    const auto first = store.attachForToken(11), second = store.attachForToken(22);
    EXPECT_TRUE(store.segment(first)->transport.control.unknown);
    EXPECT_TRUE(store.segment(first)->transport.pump.unknown);

    TransportUpdate observed;
    observed.instance = a;
    observed.token = 11;
    observed.status.snapshotMs = 2000;
    observed.status.snapshotSequence = 2;
    observed.status.control = {3, 2, 1, false};
    observed.status.pump = {7, 5, 4, true};
    observed.status.controllerDroppedControl = 8;
    observed.status.controllerDroppedPump = 9;
    observed.status.statistics.known = true;
    observed.status.statistics.methods[static_cast<size_t>(MonitorMethod::CaptureGetBuffer)] = {4096, 3, 2, false};
    PipelineAttachController::Batch progress;
    progress.transport = {observed};
    // API failure and audio discontinuity are unrelated to transport loss.
    auto failed = segmentCall(a, 11, "failure");
    failed.hresult = static_cast<int32_t>(0x80004005);
    failed.xrun = true;
    progress.calls = {failed};
    selection.apply(store, std::move(progress));
    EXPECT_EQ(store.segment(first)->transport.control.overwritten, 3u);
    EXPECT_EQ(store.segment(first)->transport.control.writeRejected, 2u);
    EXPECT_EQ(store.segment(first)->transport.control.pending, 1u);
    EXPECT_FALSE(store.segment(first)->transport.control.unknown);
    EXPECT_EQ(store.segment(first)->transport.pump.overwritten, 7u);
    EXPECT_EQ(store.segment(first)->transport.pump.pending, 4u);
    EXPECT_TRUE(store.segment(first)->transport.pump.unknown);
    EXPECT_EQ(store.segment(first)->transport.controllerDroppedControl, 8u);
    EXPECT_EQ(store.segment(first)->transport.controllerDroppedPump, 9u);
    const auto& statistics = store.segment(first)->transport.statistics;
    EXPECT_TRUE(statistics.known);
    EXPECT_EQ(statistics.methods[static_cast<size_t>(MonitorMethod::CaptureGetBuffer)].completed, 4096u);
    EXPECT_EQ(statistics.methods[static_cast<size_t>(MonitorMethod::CaptureGetBuffer)].failed, 3u);
    EXPECT_EQ(statistics.methods[static_cast<size_t>(MonitorMethod::CaptureGetBuffer)].discontinuities, 2u);
    EXPECT_TRUE(store.segment(second)->transport.control.unknown);
    EXPECT_EQ(store.segment(second)->transport.control.overwritten, 0u);
    PipelineAttachController::Batch invalid;
    auto wrongInstance = observed;
    wrongInstance.instance.created = 999;
    wrongInstance.status.snapshotMs = 3000;
    wrongInstance.status.snapshotSequence = 3;
    wrongInstance.status.control.overwritten = 99;
    auto stale = observed;
    stale.status.snapshotMs = 1999;
    stale.status.snapshotSequence = 1;
    stale.status.control.overwritten = 98;
    invalid.transport = {wrongInstance, stale};
    selection.apply(store, std::move(invalid));
    EXPECT_EQ(store.segment(first)->transport.snapshotMs, 2000);
    EXPECT_EQ(store.segment(first)->transport.control.overwritten, 3u);

    PipelineAttachController::Batch ended;
    ended.events = {segmentEvent(a, 11, true)};
    observed.status.snapshotMs = 2400;
    observed.status.snapshotSequence = 3;
    observed.status.control.pending = 2;
    observed.status.control.unknown = true;
    ended.transport = {observed};
    selection.apply(store, std::move(ended));
    EXPECT_EQ(store.segment(first)->snapshotMs, 2500);
    EXPECT_EQ(store.segment(first)->transport.snapshotMs, 2400);
    EXPECT_EQ(store.segment(first)->transport.control.pending, 2u);
    EXPECT_TRUE(store.segment(first)->transport.control.unknown);

    PipelineAttachController::Batch reattached;
    reattached.events = {segmentEvent(a, 33)};
    observed.status.snapshotMs = 9999;
    observed.status.snapshotSequence = 4;
    observed.status.control.overwritten = 99;
    reattached.transport = {observed};
    selection.apply(store, std::move(reattached));
    const auto current = store.attachForToken(33);
    EXPECT_NE(first, current);
    EXPECT_EQ(store.segment(first)->transport.snapshotMs, 2400);
    EXPECT_EQ(store.segment(first)->transport.control.overwritten, 3u);
    EXPECT_TRUE(store.segment(current)->transport.control.unknown);
    EXPECT_EQ(store.segment(current)->transport.control.overwritten, 0u);
    EXPECT_EQ(store.segment(first)->transport.statistics.methods[static_cast<size_t>(MonitorMethod::CaptureGetBuffer)].completed, 4096u);
    EXPECT_FALSE(store.segment(current)->transport.statistics.known);
    EXPECT_EQ(selection.selected(), first);
}

TEST(PipelineAttachUiModel, ClockRollbackKeepsTheLatestTransportAndFinalSnapshot) {
    CallLogStore store;
    pipeline_attach_ui::SegmentSelection selection;
    const ProcessInstance instance{7, 100};
    PipelineAttachController::Batch attached;
    attached.events = {segmentEvent(instance, 11)};
    selection.apply(store, std::move(attached));
    const auto id = store.attachForToken(11);

    TransportUpdate observed;
    observed.instance = instance;
    observed.token = 11;
    observed.status.snapshotMs = 2000;
    observed.status.snapshotSequence = 1;
    observed.status.control = {3, 2, 1, false};
    PipelineAttachController::Batch progress;
    progress.transport = {observed};
    selection.apply(store, std::move(progress));

    observed.status.snapshotMs = 1900;
    observed.status.snapshotSequence = 2;
    observed.status.control.overwritten = 4;
    PipelineAttachController::Batch rolledBack;
    rolledBack.transport = {observed};
    selection.apply(store, std::move(rolledBack));
    EXPECT_EQ(store.segment(id)->transport.snapshotMs, 1900);
    EXPECT_EQ(store.segment(id)->transport.control.overwritten, 4u);

    // A later wall time must not make a duplicate or older snapshot current.
    auto stale = observed;
    stale.status.snapshotMs = 9000;
    stale.status.control.overwritten = 99;
    EXPECT_FALSE(store.updateTransport(stale));
    stale.status.snapshotSequence = 1;
    EXPECT_FALSE(store.updateTransport(stale));
    stale.status.snapshotSequence = 3;
    stale.token = 22;
    EXPECT_FALSE(store.updateTransport(stale));
    EXPECT_EQ(store.segment(id)->transport.snapshotMs, 1900);
    EXPECT_EQ(store.segment(id)->transport.control.overwritten, 4u);

    observed.status.snapshotMs = 1800;
    observed.status.snapshotSequence = 3;
    observed.status.control = {5, 3, 2, true};
    observed.status.pump = {7, 4, 1, true};
    PipelineAttachController::Batch ended;
    auto event = segmentEvent(instance, 11, true);
    event.snapshotMs = 1800;
    event.exited = true;
    ended.events = {event};
    ended.transport = {observed};
    selection.apply(store, std::move(ended));
    EXPECT_EQ(store.segment(id)->state, CallLogStore::SegmentState::Exited);
    EXPECT_EQ(store.segment(id)->snapshotMs, 1800);
    EXPECT_EQ(store.segment(id)->transport.snapshotMs, 1800);
    EXPECT_EQ(store.segment(id)->transport.control.overwritten, 5u);
    EXPECT_EQ(store.segment(id)->transport.control.writeRejected, 3u);
    EXPECT_EQ(store.segment(id)->transport.control.pending, 2u);
    EXPECT_TRUE(store.segment(id)->transport.control.unknown);
    EXPECT_EQ(store.segment(id)->transport.pump.overwritten, 7u);
    EXPECT_TRUE(store.segment(id)->transport.pump.unknown);

    observed.status.snapshotMs = 9999;
    observed.status.snapshotSequence = 4;
    observed.status.control = {99, 0, 0, false};
    PipelineAttachController::Batch late;
    late.transport = {observed};
    selection.apply(store, std::move(late));
    EXPECT_EQ(store.segment(id)->transport.snapshotMs, 1800);
    EXPECT_EQ(store.segment(id)->transport.control.overwritten, 5u);
    EXPECT_TRUE(store.segment(id)->transport.control.unknown);
    EXPECT_EQ(selection.selected(), id);
}

TEST(PipelineAttachUiModel, ReceivedPumpIncrementsKeepRecentHistoryWithoutInventingLoss) {
    CallLogStore store;
    store.setPumpVisible(true);
    pipeline_attach_ui::SegmentSelection selection;
    const ProcessInstance instance{7, 100};
    PipelineAttachController::Batch attached;
    attached.events = {segmentEvent(instance, 11)};
    selection.apply(store, std::move(attached));
    const auto id = store.attachForToken(11);
    auto pump = segmentCall(instance, 11, "frame");
    pump.iface = "IAudioRenderClient";
    pump.method = "GetBuffer";
    pump.pump = true;
    PipelineAttachController::Batch first;
    first.pump = {{11, {{1, pump}, {2, pump}}}};
    selection.apply(store, std::move(first));
    PipelineAttachController::Batch second;
    second.pump = {{11, {{3, pump}}}};
    selection.apply(store, std::move(second));
    call_log_filter::FilterOpts query;
    query.attach = id;
    ASSERT_EQ(store.view(query).size(), 3u);
    EXPECT_EQ(store.view(query).at(0)->id.sequence, 1u);
    EXPECT_EQ(store.view(query).at(2)->id.sequence, 3u);
    PipelineAttachController::Batch duplicate;
    duplicate.pump = {{11, {{3, pump}}}};
    selection.apply(store, std::move(duplicate));
    EXPECT_EQ(store.view(query).size(), 3u);
    for (uint64_t sequence = 4; sequence <= 70; ++sequence) {
        PipelineAttachController::Batch next;
        next.pump = {{11, {{sequence, pump}}}};
        selection.apply(store, std::move(next));
    }
    const auto recent = store.view(query);
    ASSERT_EQ(recent.size(), 70u);
    EXPECT_EQ(recent.at(0)->id.sequence, 1u);
    EXPECT_EQ(recent.at(69)->id.sequence, 70u);
    PipelineAttachController::Batch late;
    late.pump = {{11, {{1, pump}}}};
    selection.apply(store, std::move(late));
    EXPECT_EQ(store.view(query).size(), 70u);
    EXPECT_EQ(store.view(query).at(0)->id.sequence, 1u);
    EXPECT_EQ(store.segment(id)->transport.pump.overwritten, 0u);
    EXPECT_EQ(store.segment(id)->transport.controllerDroppedPump, 0u);
    EXPECT_TRUE(store.segment(id)->transport.pump.unknown);
}

TEST(PipelineAttachUiModel, PresentationHandoffRunsAtMostTenTimesPerSecond) {
    pipeline_attach_ui::PresentationRefresh refresh;
    size_t accepted = 0;
    for (uint64_t millisecond = 0; millisecond < 1000; ++millisecond)
        if (refresh.take(millisecond)) ++accepted;
    EXPECT_EQ(accepted, 10u);
    EXPECT_TRUE(refresh.take(1000));
    EXPECT_FALSE(refresh.take(1099));
    EXPECT_TRUE(refresh.take(1100));
}

TEST(PipelineAttachUiModel, ImmediateClearFeedbackIsDeliveredOnceWithoutReplayingOldQueuedData) {
    using namespace pipeline_attach_ui;
    const ProcessInstance instance{7, 100};
    CallLogStore store;
    store.setPumpVisible(true);
    SegmentSelection selection;
    PipelineAttachController::Batch initial;
    auto attached = segmentEvent(instance, 11);
    attached.observationGeneration = 1;
    initial.events = {attached};
    auto old = segmentCall(instance, 11, "old");
    old.observationGeneration = 1;
    initial.calls = {old};
    selection.apply(store, std::move(initial));
    const auto id = selection.selected();
    auto cleared = attached;
    cleared.kind = PipelineAttachController::EventKind::Cleared;
    cleared.observationGeneration = 2;
    auto configured = cleared;
    configured.kind = PipelineAttachController::EventKind::Configured;
    configured.methods = methodBit(MonitorMethod::GetFrequency);
    auto fresh = old;
    fresh.args = "fresh";
    fresh.observationGeneration = 2;
    PipelineAttachController::Batch queued;
    queued.events = {cleared, configured};
    queued.calls = {old, fresh};
    auto oldPump = old, freshPump = fresh;
    oldPump.pump = freshPump.pump = true;
    oldPump.iface = freshPump.iface = "IAudioClock";
    oldPump.method = freshPump.method = "GetFrequency";
    queued.pump = {{11, {{99, oldPump}, {1, freshPump}}}};
    TransportUpdate stale{instance, 11, {}};
    stale.status.observationGeneration = 1;
    stale.status.snapshotSequence = 999;
    stale.status.statistics.known = true;
    stale.status.statistics.methods[19].completed = 99;
    queued.transport = {stale};
    auto feedback = queued.takeControlFeedback();
    ASSERT_EQ(feedback.size(), 2u);
    EXPECT_EQ(feedback[0].kind, PipelineAttachController::EventKind::Cleared);
    EXPECT_EQ(feedback[1].kind, PipelineAttachController::EventKind::Configured);
    EXPECT_TRUE(queued.takeControlFeedback().empty());
    EXPECT_EQ(queued.calls.size(), 2u);
    for (auto& event : feedback) {
        PipelineAttachController::Batch completion;
        completion.events.push_back(std::move(event));
        selection.apply(store, std::move(completion));
    }
    EXPECT_EQ(store.segment(id)->observationGeneration, 2u);
    EXPECT_EQ(store.segment(id)->methods, methodBit(MonitorMethod::GetFrequency));
    EXPECT_EQ(store.segment(id)->count, 0u);
    selection.apply(store, std::move(queued));
    ASSERT_EQ(store.segment(id)->count, 1u);
    EXPECT_EQ(store.entries().front()->call.args, "fresh");
    EXPECT_EQ(store.segment(id)->windowRetained, 1u);
    EXPECT_EQ(store.segment(id)->windowReceived, 1u);
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[19].completed, 0u);
    auto current = stale;
    current.status.observationGeneration = 2;
    current.status.snapshotSequence = 1;
    current.status.statistics.methods[19].completed = 1;
    ASSERT_TRUE(store.updateTransport(current));
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[19].completed, 1u);
    EXPECT_EQ(store.segment(id)->configurationChanges.size(), 1u);
    EXPECT_EQ(selection.selected(), id);
}

TEST(PipelineAttachUiModel, FastFeedbackPreservesLifecycleBarriersAndFinalSnapshots) {
    for (const auto kind : {PipelineAttachController::EventKind::Attached,
                           PipelineAttachController::EventKind::Detached,
                           PipelineAttachController::EventKind::Failed}) {
        PipelineAttachController::Batch batch;
        auto lifecycle = segmentEvent({7, 100}, 11);
        lifecycle.kind = kind;
        auto configured = lifecycle;
        configured.kind = PipelineAttachController::EventKind::Configured;
        batch.events = {lifecycle, configured};
        batch.calls = {segmentCall({7, 100}, 11, "bounded tail")};
        batch.transport = {{{7, 100}, 11, {}}};
        batch.transport.front().status.snapshotSequence = 17;
        EXPECT_TRUE(batch.takeControlFeedback().empty());
        ASSERT_EQ(batch.events.size(), 2u);
        EXPECT_EQ(batch.events.front().kind, kind);
        EXPECT_EQ(batch.calls.front().args, "bounded tail");
        EXPECT_EQ(batch.transport.front().status.snapshotSequence, 17u);
    }
}

TEST(PipelineAttachUiModel, ControllerFeedbackDeliversCommandFailuresOnceWithoutOrdinaryPolling) {
    PipelineAttachController controller;
    ASSERT_TRUE(controller.configure({7, 100}, 11, kDefaultMonitorMethods));
    ASSERT_TRUE(controller.clear({7, 100}, 11));
    std::vector<PipelineAttachController::Event> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (events.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        auto next = controller.pollControlFeedback();
        events.insert(events.end(), next.begin(), next.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, PipelineAttachController::EventKind::ConfigurationFailed);
    EXPECT_EQ(events[1].kind, PipelineAttachController::EventKind::ClearFailed);
    EXPECT_EQ(events[0].instance, (ProcessInstance{7, 100}));
    EXPECT_EQ(events[1].token, 11u);
    EXPECT_FALSE(events[0].result);
    EXPECT_FALSE(events[1].result);
    EXPECT_TRUE(controller.pollControlFeedback().empty());
    for (const auto& batch : controller.poll()) EXPECT_TRUE(batch.events.empty());
}

TEST(PipelineAttachUiModel, SuccessfulClearRejectsQueuedOldRowsAndStatisticsWithoutChangingSelection) {
    CallLogStore store;
    store.setPumpVisible(true);
    pipeline_attach_ui::SegmentSelection selection;
    const ProcessInstance a{7, 100}, b{9, 200};
    PipelineAttachController::Batch initial;
    auto attached = segmentEvent(a, 11);
    attached.observationGeneration = 1;
    initial.events = {attached, segmentEvent(b, 22)};
    auto old = segmentCall(a, 11, "old");
    old.observationGeneration = 1;
    initial.calls = {old, segmentCall(b, 22, "other")};
    selection.apply(store, std::move(initial));
    const auto id = selection.selected();
    TransportUpdate status{a, 11, {}};
    status.status.snapshotSequence = 1;
    status.status.observationGeneration = 1;
    status.status.statistics.known = true;
    status.status.statistics.methods[0].completed = 27;
    ASSERT_TRUE(store.updateTransport(status));
    auto publication = attached;
    publication.kind = PipelineAttachController::EventKind::Cleared;
    publication.observationGeneration = 2;
    publication.snapshotMs = 5000;
    PipelineAttachController::Batch cleared;
    cleared.events = {publication};
    cleared.calls = {old};
    old.pump = true;
    old.method = "GetBuffer";
    cleared.pump = {{11, {{1, old}}}};
    status.status.snapshotSequence = 99;
    cleared.transport = {status};
    selection.apply(store, std::move(cleared));
    EXPECT_EQ(selection.selected(), id);
    EXPECT_EQ(store.segment(id)->count, 0u);
    EXPECT_EQ(store.segment(id)->windowRetained, 0u);
    EXPECT_EQ(store.segment(id)->windowReceived, 0u);
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[0].completed, 0u);
    EXPECT_EQ(store.segment(id)->statisticsStartedMs, 5000);
    EXPECT_EQ(store.segment(id)->methods, kDefaultMonitorMethods);
    EXPECT_EQ(store.segment(store.attachForToken(22))->count, 1u);
    auto fresh = segmentCall(a, 11, "fresh");
    fresh.observationGeneration = 2;
    PipelineAttachController::Batch next;
    next.calls = {fresh};
    status.status.observationGeneration = 2;
    status.status.snapshotSequence = 2;
    status.status.statistics.methods[0].completed = 1;
    next.transport = {status};
    selection.apply(store, std::move(next));
    EXPECT_EQ(store.segment(id)->count, 1u);
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[0].completed, 1u);
}

TEST(PipelineAttachUiModel, FailedClearKeepsStatisticsAndSuccessfulClearLeavesOtherActiveAndHistoricalSegments) {
    CallLogStore store;
    store.setPumpVisible(true);
    pipeline_attach_ui::SegmentSelection selection;
    const ProcessInstance a{7, 100}, b{9, 200}, c{10, 300};
    PipelineAttachController::Batch initial;
    auto event = segmentEvent(a, 11);
    event.methods = methodBit(MonitorMethod::SetMute);
    initial.events = {event, segmentEvent(b, 22), segmentEvent(c, 33)};
    initial.calls = {segmentCall(a, 11, "A"), segmentCall(b, 22, "B"), segmentCall(c, 33, "C")};
    selection.apply(store, std::move(initial));
    const auto id = selection.selected(), other = store.attachForToken(22), history = store.attachForToken(33);
    const auto keptB = store.entries()[1], keptC = store.entries()[2];
    PipelineAttachController::Batch end;
    end.events = {segmentEvent(c, 33, true)};
    selection.apply(store, std::move(end));
    TransportUpdate status{a, 11, {}};
    status.status.snapshotSequence = 1;
    status.status.snapshotMs = 2000;
    status.status.control = {5, 3, 2, true};
    status.status.statistics.known = true;
    status.status.statistics.methods[0].completed = 123;
    ASSERT_TRUE(store.updateTransport(status));
    auto failed = event;
    failed.kind = PipelineAttachController::EventKind::ClearFailed;
    failed.result = Result::Fail(static_cast<long>(0x80004005), "clear failed");
    failed.snapshotMs = 3000;
    PipelineAttachController::Batch failure;
    failure.events = {failed};
    selection.apply(store, std::move(failure));
    EXPECT_EQ(store.segment(id)->count, 1u);
    EXPECT_EQ(store.segment(id)->statisticsStartedMs, 1000);
    EXPECT_EQ(store.segment(id)->transport.statistics.methods[0].completed, 123u);
    EXPECT_EQ(store.segment(id)->transport.control.overwritten, 5u);
    auto success = failed;
    success.kind = PipelineAttachController::EventKind::Cleared;
    success.result = Result::Ok();
    success.observationGeneration = 1;
    PipelineAttachController::Batch cleared;
    cleared.events = {success};
    selection.apply(store, std::move(cleared));
    EXPECT_EQ(selection.selected(), id);
    EXPECT_EQ(store.segment(id)->methods, methodBit(MonitorMethod::SetMute));
    EXPECT_EQ(store.segment(id)->transport.control.overwritten, 0u);
    EXPECT_TRUE(store.segment(id)->transport.control.unknown);
    EXPECT_TRUE(store.segment(id)->coverageUnknown);
    EXPECT_EQ(store.segment(other)->count, 1u);
    EXPECT_EQ(store.segment(history)->count, 1u);
    EXPECT_EQ(store.segment(history)->state, CallLogStore::SegmentState::Detached);
    EXPECT_EQ(store.find(keptB->id), keptB.get());
    EXPECT_EQ(store.find(keptC->id), keptC.get());
    ASSERT_TRUE(selection.select(store, history));
    EXPECT_TRUE(store.clearSegment(history, 0, 4000));
    EXPECT_EQ(selection.selected(), history);
    EXPECT_EQ(store.segment(history)->count, 0u);
    EXPECT_EQ(store.segment(history)->instance, c);
    EXPECT_EQ(store.segment(other)->count, 1u);
}
