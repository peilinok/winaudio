#include <gtest/gtest.h>
#include "CallLogExportJob.h"
#include "PipelineAttachUiModel.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>

using namespace wa;

namespace {
class ControlledWriter : public CallLogWriter {
public:
    int fail = -1;
    bool cleanupFailure = false, block = false;
    std::string content;
    std::vector<int> stages;
    Result step(int stage) {
        stages.push_back(stage);
        return fail == stage ? Result::Fail(1, "injected failure") : Result::Ok();
    }
    Result open(const std::wstring&) override { return step(0); }
    Result write(std::string_view bytes) override {
        if (block) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return released; });
        }
        content.append(bytes);
        return step(1);
    }
    Result flush() override { return step(2); }
    Result close() override { return step(3); }
    Result publish() override { return step(4); }
    Result cleanup() override { step(5); return cleanupFailure ? Result::Fail(2, "cleanup failure") : Result::Ok(); }
    bool waitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
private:
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
};

HookedCall row() {
    HookedCall call;
    call.pid = 7;
    call.method = "Initialize";
    call.args = "line1\nline2,\"value\"";
    call.parameters.push_back({0, 0, "bool", "Raw", "", "false"});
    return call;
}
}

TEST(CallLogExport, CurrentSegmentIncludesHiddenWindowAndIgnoresRecordingAndDisplayFilters) {
    CallLogStore store;
    const auto old = store.registerAttach({7, 100}, "A", 11, 1000);
    store.append(old, row());
    store.endAttach(old, false, 1100);
    const auto a = store.registerAttach({7, 100}, "A", 12, 1200);
    const auto b = store.registerAttach({9, 200}, "B", 21, 1200);
    store.setRecording(true);
    store.setRecording(false);
    store.append(a, row());
    auto other = row();
    other.pid = 9;
    store.append(b, other);
    auto pump = row();
    pump.iface = "IAudioClock";
    pump.method = "GetFrequency";
    pump.timeMs = 1300;
    store.appendPump(a, {{1, pump}});
    EXPECT_EQ(store.view({"no-match", "", false, b}).size(), 0u);
    const auto snapshot = store.freezeExport(a, 1500);
    ASSERT_EQ(snapshot.records.size(), 2u);
    EXPECT_EQ(snapshot.records[0]->id.attach, a);
    EXPECT_EQ(snapshot.records[0]->call.method, "Initialize");
    EXPECT_EQ(snapshot.records[1]->id.attach, a);
    EXPECT_EQ(snapshot.records[1]->id.lane, CallLane::Pump);
    EXPECT_EQ(snapshot.records[1]->call.method, "GetFrequency");
}

TEST(CallLogExport, FrozenReferencesStayStableWhileGuiContinuesAndJobIsSingle) {
    CallLogStore store;
    store.append({row()});
    auto writer = std::make_shared<ControlledWriter>();
    writer->block = true;
    CallLogExportJob job(writer);
    ASSERT_TRUE(job.start(store.freezeExport(), L"ignored.jsonl"));
    const bool entered = writer->waitEntered();
    EXPECT_TRUE(entered);
    EXPECT_TRUE(job.busy());
    EXPECT_FALSE(job.start(store.freezeExport(), L"another.jsonl"));
    store.setRecording(true);
    store.append({row()});
    store.setRecording(false);
    EXPECT_EQ(store.view({}).size(), 2u);
    CallLogExportResult result;
    EXPECT_FALSE(job.take(result));
    writer->release();
    ASSERT_TRUE(job.wait(result));
    EXPECT_TRUE(result.result);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.recordsWritten, 1u);
    EXPECT_NE(writer->content.find("\"name\":\"Raw\""), std::string::npos);
    EXPECT_EQ(std::count(writer->content.begin(), writer->content.end(), '\n'), 1);
    EXPECT_FALSE(job.busy());
    EXPECT_EQ(store.exportCount(), 2u);
}

TEST(CallLogExport, SummaryOnlyExportOwnsStatisticsAndConfigurationAcrossClearAndReattach) {
    CallLogStore store;
    const auto a = store.registerAttach({7, 100}, "A,\"quoted\"\nname", 11, 1000, 0);
    ASSERT_TRUE(store.setObservationGeneration(a, 1));
    ASSERT_TRUE(store.setMethods(a, methodBit(MonitorMethod::GetFrequency), 1100));
    HookTransportStatus status;
    status.observationGeneration = 1;
    status.snapshotMs = 1200;
    status.snapshotSequence = 3;
    status.control = {4, 5, 6, true};
    status.pump = {7, 8, 9, false};
    status.controllerDroppedControl = 10;
    status.controllerDroppedPump = 11;
    status.statistics.known = true;
    status.statistics.methods[static_cast<size_t>(MonitorMethod::GetFrequency)] = {45, 2, 0, false};
    ASSERT_TRUE(store.updateTransport({{7, 100}, 11, status}));
    const auto snapshot = store.freezeExport(a, 1300);
    ASSERT_TRUE(snapshot.segment);
    EXPECT_EQ(snapshot.segment->initialMethods, 0u);
    ASSERT_EQ(snapshot.segment->configurationChanges.size(), 1u);
    EXPECT_EQ(snapshot.segment->configurationChanges[0].observedMs, 1100);
    EXPECT_EQ(snapshot.segment->configurationChanges[0].observationGeneration, 1u);
    EXPECT_EQ(snapshot.frozenMs, 1300);
    auto writer = std::make_shared<ControlledWriter>();
    writer->block = true;
    CallLogExportJob job(writer);
    ASSERT_TRUE(job.start(snapshot, L"ignored.jsonl"));
    EXPECT_TRUE(writer->waitEntered());
    EXPECT_TRUE(store.clearSegment(a, 2, 1400));
    store.endAttach(a, true, 1500);
    store.registerAttach({7, 100}, "A", 12, 1600);
    store.beginRound();
    writer->release();
    CallLogExportResult result;
    ASSERT_TRUE(job.wait(result));
    EXPECT_TRUE(result.result);
    EXPECT_EQ(result.recordsWritten, 0u);
    EXPECT_EQ(snapshot.segment->transport.snapshotMs, 1200);
    EXPECT_EQ(snapshot.segment->transport.pump.overwritten, 7u);
    EXPECT_EQ(snapshot.segment->state, CallLogStore::SegmentState::Monitoring);
    EXPECT_NE(writer->content.find("\"rowType\":\"segment\""), std::string::npos);
    EXPECT_NE(writer->content.find("\"statistics.IAudioClock.GetFrequency.completed\":45"), std::string::npos);
}

TEST(CallLogExport, InFlightSegmentKeepsCallsWindowAndSummaryWhenSelectionAndStoreChange) {
    CallLogStore store;
    pipeline_attach_ui::SegmentSelection selection;
    const auto a = store.registerAttach({7, 100}, "A", 11, 1000);
    const auto b = store.registerAttach({9, 200}, "B", 22, 1000);
    ASSERT_TRUE(selection.select(store, a));
    store.append(a, row());
    auto pump = row();
    pump.iface = "IAudioClock";
    pump.method = "GetFrequency";
    store.appendPump(a, {{1, pump}});
    const auto snapshot = store.freezeExport(selection.selected(), 1500);
    auto writer = std::make_shared<ControlledWriter>();
    writer->block = true;
    CallLogExportJob job(writer);
    ASSERT_TRUE(job.start(snapshot, L"ignored.jsonl"));
    EXPECT_TRUE(writer->waitEntered());
    EXPECT_TRUE(selection.select(store, b));
    store.append(a, row());
    for (uint64_t i = 2; i <= 1030; ++i) store.appendPump(a, {{i, pump}});
    EXPECT_TRUE(store.clearSegment(a, 1, 2000));
    store.endAttach(a, true, 2100);
    store.registerAttach({7, 100}, "A", 12, 2200);
    store.beginRound();
    writer->release();
    CallLogExportResult result;
    ASSERT_TRUE(job.wait(result));
    EXPECT_TRUE(result.result);
    EXPECT_EQ(result.recordsWritten, 2u);
    EXPECT_EQ(std::count(writer->content.begin(), writer->content.end(), '\n'), 3);
    EXPECT_NE(writer->content.find("\"windowReceived\":1"), std::string::npos);
    EXPECT_NE(writer->content.find("\"windowEvicted\":0"), std::string::npos);
    EXPECT_NE(writer->content.find("\"state\":\"monitoring\""), std::string::npos);
    EXPECT_NE(writer->content.find("\"lane\":\"pump\",\"sequence\":1"), std::string::npos);
    EXPECT_EQ(writer->content.find("\"pid\":9"), std::string::npos);
}

TEST(CallLogExport, ConfigurationChangeAtAttachTimestampIsStillADistinctObservedBoundary) {
    CallLogStore store;
    const auto a = store.registerAttach({7, 100}, "A", 11, 1000);
    ASSERT_TRUE(store.setMethods(a, 0, 1000));
    const auto frozen = store.freezeExport(a, 1100);
    ASSERT_TRUE(frozen.segment);
    EXPECT_EQ(frozen.segment->initialMethods, kDefaultMonitorMethods);
    ASSERT_EQ(frozen.segment->configurationChanges.size(), 1u);
    EXPECT_EQ(frozen.segment->configurationChanges[0].methods, 0u);
    EXPECT_EQ(frozen.segment->configurationChanges[0].observedMs, 1000);
}

TEST(CallLogExport, ConfigurationEventKeepsItsGenerationWhenClearFollowsInTheSameBatch) {
    CallLogStore store;
    pipeline_attach_ui::SegmentSelection selection;
    PipelineAttachController::Event attached;
    attached.kind = PipelineAttachController::EventKind::Attached;
    attached.instance = {7, 100};
    attached.pid = 7;
    attached.token = 11;
    attached.snapshotMs = 1000;
    attached.observationGeneration = 1;
    PipelineAttachController::Batch initial;
    initial.events = {attached};
    selection.apply(store, std::move(initial));
    auto configured = attached;
    configured.kind = PipelineAttachController::EventKind::Configured;
    configured.methods = methodBit(MonitorMethod::GetFrequency);
    configured.snapshotMs = 1100;
    auto cleared = configured;
    cleared.kind = PipelineAttachController::EventKind::Cleared;
    cleared.observationGeneration = 2;
    cleared.snapshotMs = 1200;
    PipelineAttachController::Batch sameBatch;
    sameBatch.events = {configured, cleared};
    selection.apply(store, std::move(sameBatch));
    const auto snapshot = store.freezeExport(selection.selected(), 1300);
    ASSERT_TRUE(snapshot.segment);
    ASSERT_EQ(snapshot.segment->configurationChanges.size(), 1u);
    EXPECT_EQ(snapshot.segment->observationGeneration, 2u);
    EXPECT_EQ(snapshot.segment->configurationChanges[0].observationGeneration, 1u);
    EXPECT_EQ(snapshot.segment->configurationChanges[0].observedMs, 1100);
    EXPECT_EQ(snapshot.segment->methods, methodBit(MonitorMethod::GetFrequency));
    auto writer = std::make_shared<ControlledWriter>();
    CallLogExportJob job(writer);
    ASSERT_TRUE(job.start(snapshot, L"ignored.jsonl"));
    CallLogExportResult result;
    ASSERT_TRUE(job.wait(result));
    ASSERT_TRUE(result.result);
    EXPECT_NE(writer->content.find("\"configurationChanges.0.observationGeneration\":1"), std::string::npos);
    EXPECT_NE(writer->content.find("\"observationGeneration\":2"), std::string::npos);
}

TEST(CallLogExport, RealSegmentFilesPreserveFullPayloadAndFlatSummaryForIndependentParsing) {
    const auto folder = std::filesystem::temp_directory_path() / L"winaudio-segment-export-test";
    std::filesystem::create_directories(folder);
    CallLogStore store;
    const auto old = store.registerAttach({7, 100}, "old", 10, 1000);
    store.append(old, row());
    const auto b = store.registerAttach({9, 200}, "B", 20, 2000);
    auto foreign = row();
    foreign.pid = 9;
    store.append(b, foreign);
    store.endAttach(old, true, 2500);
    const auto a = store.registerAttach({7, 100}, "A,\"quoted\"\nname", 30, 3000, 0);
    ASSERT_TRUE(store.setObservationGeneration(a, 1));
    ASSERT_TRUE(store.setMethods(a, methodBit(MonitorMethod::GetFrequency), 3050));
    ASSERT_TRUE(store.setMethods(a, kAllMonitorMethods, 3060));
    store.setRecording(true);
    store.setRecording(false);
    auto control = row();
    control.observationGeneration = 1;
    control.timeMs = 3100;
    control.iface = "IAudioClient";
    control.hresult = static_cast<int32_t>(0x80004005);
    control.streamId = 42;
    control.category = "Media";
    control.format = "48000/32f/2";
    control.exclusive = false;
    control.raw = true;
    control.matchFormat = false;
    store.append(a, control);
    auto pump = control;
    pump.iface = "IAudioClock";
    pump.method = "GetFrequency";
    pump.args = "frequency=48000";
    pump.parameters.clear();
    pump.hresult = 1;
    pump.xrun = true;
    std::vector<PumpCall> calls;
    for (uint64_t i = 1; i <= 1030; ++i) {
        pump.timeMs = 4000 + static_cast<int64_t>(i);
        calls.push_back({i, pump});
    }
    store.appendPump(a, calls);
    HookTransportStatus status;
    status.observationGeneration = 1;
    status.snapshotMs = 7000;
    status.snapshotSequence = 9;
    status.control = {4, 5, 6, true};
    status.pump = {7, 8, 9, false};
    status.controllerDroppedControl = 10;
    status.controllerDroppedPump = 11;
    status.statistics.known = true;
    status.statistics.methods[static_cast<size_t>(MonitorMethod::GetFrequency)] = {2000, 17, 3, true};
    ASSERT_TRUE(store.updateTransport({{7, 100}, 30, status}));
    store.endAttach(a, false, 7500);
    EXPECT_EQ(store.view({"no-match", "", false, b}).size(), 0u);
    const auto frozen = store.freezeExport(a, 8000);
    for (const bool csv : {false, true}) {
        const auto path = folder / (csv ? L"segment.csv" : L"segment.jsonl");
        std::filesystem::remove(path);
        CallLogExportJob job;
        ASSERT_TRUE(job.start(frozen, path.wstring(), csv));
        CallLogExportResult result;
        ASSERT_TRUE(job.wait(result));
        ASSERT_TRUE(result.result) << result.result.message;
        EXPECT_EQ(result.recordsWritten, 1025u);
    }
    const auto empty = store.registerAttach({8, 300}, "empty", 40, 9000);
    for (const bool csv : {false, true}) {
        const auto path = folder / (csv ? L"summary-only.csv" : L"summary-only.jsonl");
        std::filesystem::remove(path);
        CallLogExportJob job;
        ASSERT_TRUE(job.start(store.freezeExport(empty, 9500), path.wstring(), csv));
        CallLogExportResult result;
        ASSERT_TRUE(job.wait(result));
        ASSERT_TRUE(result.result);
        EXPECT_EQ(result.recordsWritten, 0u);
    }
    status.observationGeneration = 0;
    ASSERT_TRUE(store.updateTransport({{8, 300}, 40, status}));
    for (const bool csv : {false, true}) {
        const auto path = folder / (csv ? L"statistics-only.csv" : L"statistics-only.jsonl");
        std::filesystem::remove(path);
        CallLogExportJob job;
        ASSERT_TRUE(job.start(store.freezeExport(empty, 9600), path.wstring(), csv));
        CallLogExportResult result;
        ASSERT_TRUE(job.wait(result));
        ASSERT_TRUE(result.result);
        EXPECT_EQ(result.recordsWritten, 0u);
    }
}

TEST(CallLogExport, SummaryFileFailuresAndCancellationKeepInMemorySegmentAndStatistics) {
    CallLogStore store;
    const auto a = store.registerAttach({7, 100}, "A", 11, 1000);
    HookTransportStatus status;
    status.snapshotSequence = 1;
    status.pump.overwritten = 4;
    status.statistics.known = true;
    status.statistics.methods[static_cast<size_t>(MonitorMethod::GetFrequency)].completed = 45;
    ASSERT_TRUE(store.updateTransport({{7, 100}, 11, status}));
    for (int stage = 0; stage <= 4; ++stage) {
        auto writer = std::make_shared<ControlledWriter>();
        writer->fail = stage;
        writer->cleanupFailure = stage == 1;
        CallLogExportJob job(writer);
        ASSERT_TRUE(job.start(store.freezeExport(a, 1500), L"ignored.jsonl"));
        CallLogExportResult result;
        ASSERT_TRUE(job.wait(result));
        EXPECT_FALSE(result.result) << stage;
        EXPECT_EQ(result.cleanupFailed, stage == 1);
        EXPECT_EQ(writer->stages.back(), 5);
        EXPECT_EQ(store.segment(a)->transport.pump.overwritten, 4u);
        EXPECT_EQ(store.segment(a)->transport.statistics.methods[static_cast<size_t>(MonitorMethod::GetFrequency)].completed, 45u);
        EXPECT_TRUE(store.freezeExport(a, 1600).segment);
    }
    auto writer = std::make_shared<ControlledWriter>();
    writer->block = true;
    CallLogExportJob job(writer);
    ASSERT_TRUE(job.start(store.freezeExport(a, 1700), L"ignored.jsonl"));
    EXPECT_TRUE(writer->waitEntered());
    job.cancel();
    writer->release();
    CallLogExportResult result;
    ASSERT_TRUE(job.wait(result));
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(result.result);
    EXPECT_EQ(std::count(writer->stages.begin(), writer->stages.end(), 4), 0);
    EXPECT_EQ(writer->stages.back(), 5);
    EXPECT_EQ(store.segment(a)->transport.statistics.methods[static_cast<size_t>(MonitorMethod::GetFrequency)].completed, 45u);
}

TEST(CallLogExport, CancelWaitsForCurrentWriteAndNeverPublishes) {
    CallLogStore store;
    store.append({row()});
    auto writer = std::make_shared<ControlledWriter>();
    writer->block = true;
    CallLogExportJob job(writer);
    ASSERT_TRUE(job.start(store.freezeExport(), L"ignored.jsonl"));
    EXPECT_TRUE(writer->waitEntered());
    job.cancel();
    EXPECT_TRUE(job.busy());
    writer->release();
    CallLogExportResult result;
    ASSERT_TRUE(job.wait(result));
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(result.result);
    EXPECT_EQ(std::count(writer->stages.begin(), writer->stages.end(), 4), 0);
    EXPECT_EQ(writer->stages.back(), 5);
    EXPECT_EQ(store.size(), 1u);
}

TEST(CallLogExport, ExitWaitAndCancelKeepSourcesAliveUntilWorkerIsReaped) {
    for (const bool cancel : {false, true}) {
        auto writer = std::make_shared<ControlledWriter>();
        writer->block = true;
        CallLogStore store;
        store.append({row()});
        CallLogExportJob job(writer);
        ASSERT_TRUE(job.start(store.freezeExport(), L"ignored.jsonl"));
        EXPECT_TRUE(writer->waitEntered());
        pipeline_attach_ui::ExportExitState exit;
        exit.request();
        EXPECT_TRUE(exit.prompt());
        EXPECT_FALSE(exit.ready(job.busy()));
        exit.choose(cancel);
        if (exit.cancelRequested()) job.cancel();
        EXPECT_FALSE(exit.ready(job.busy()));
        writer->release();
        CallLogExportResult result;
        ASSERT_TRUE(job.wait(result));
        EXPECT_EQ(result.cancelled, cancel);
        EXPECT_TRUE(exit.ready(job.busy()));
        EXPECT_EQ(store.size(), 1u);
    }
}

TEST(CallLogExport, EveryFileFailureIsReportedAndCleanupFailureStaysIncomplete) {
    CallLogStore store;
    store.append({row()});
    for (int stage = 0; stage <= 4; ++stage) {
        auto writer = std::make_shared<ControlledWriter>();
        writer->fail = stage;
        writer->cleanupFailure = stage == 1;
        CallLogExportJob job(writer);
        ASSERT_TRUE(job.start(store.freezeExport(), L"ignored.jsonl"));
        CallLogExportResult result;
        ASSERT_TRUE(job.wait(result));
        EXPECT_FALSE(result.result) << stage;
        EXPECT_EQ(result.cleanupFailed, stage == 1);
        EXPECT_EQ(writer->stages.back(), 5);
        EXPECT_EQ(store.exportCount(), 1u);
    }
}

TEST(CallLogExport, EmptyFrozenRangeDoesNotFallBackAndCsvKeepsOriginalEscapes) {
    CallLogStore store;
    store.setRecording(true);
    store.setRecording(false);
    store.append({row()});
    auto writer = std::make_shared<ControlledWriter>();
    CallLogExportJob job(writer);
    EXPECT_FALSE(job.start(store.freezeExport(), L"ignored.jsonl"));
    EXPECT_FALSE(job.busy());
    EXPECT_TRUE(writer->stages.empty());
    store.setRecording(true);
    ASSERT_TRUE(job.start(store.freezeExport(), L"ignored.csv", true));
    CallLogExportResult result;
    ASSERT_TRUE(job.wait(result));
    EXPECT_TRUE(result.result);
    EXPECT_EQ(writer->content, CallLogStore::toCsvHeader() + '\n' + CallLogStore::toCsvLine(row()) + '\n');
}

TEST(CallLogExport, RealFilePublishesCompletelyAndRefusesToReplaceExistingFile) {
    const auto folder = std::filesystem::temp_directory_path() / L"winaudio-export-job-test";
    std::filesystem::create_directories(folder);
    const auto path = folder / L"calls.jsonl";
    std::filesystem::remove(path);
    CallLogStore store;
    store.append({row()});
    CallLogExportJob job;
    ASSERT_TRUE(job.start(store.freezeExport(), path.wstring()));
    CallLogExportResult result;
    ASSERT_TRUE(job.wait(result));
    ASSERT_TRUE(result.result) << result.result.message;
    std::ifstream file(path, std::ios::binary);
    const std::string expected = CallLogStore::toJsonLine(*store.entries().front()) + '\n';
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), expected);
    file.close();
    ASSERT_TRUE(job.start(store.freezeExport(), path.wstring()));
    ASSERT_TRUE(job.wait(result));
    EXPECT_FALSE(result.result);
    std::ifstream existing(path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(existing), {}), expected);
    existing.close();
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) { (void)entry; ++files; }
    EXPECT_EQ(files, 1u);
    std::filesystem::remove(path);
    std::filesystem::remove(folder);
}
