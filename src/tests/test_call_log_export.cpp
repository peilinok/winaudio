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
