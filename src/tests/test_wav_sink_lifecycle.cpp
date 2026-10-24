#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <gtest/gtest.h>
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <system_error>
#include <vector>
#include "WavFile.h"
#include "WavSink.h"
#include "WavSinkTestHooks.h"

using namespace wa;

namespace {
using Point = wavSinkTest::Point;
constexpr auto kDeadlockTimeout = std::chrono::seconds(10);

struct Schedule {
    std::mutex mtx;
    std::condition_variable cv;
    const WavSink* sink = nullptr;
    Point point = Point::AfterAdmission;
    std::thread::id producer;
    bool paused = false;
    bool released = false;
    bool drainSeen = false;
    bool drainActive = false;
    bool closeEntered = false;
    bool closeReleased = false;

    template<class Pred> bool wait(Pred pred) {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, kDeadlockTimeout, pred);
    }
    void releaseProducer() {
        std::lock_guard<std::mutex> lk(mtx);
        released = true;
        cv.notify_all();
    }
    void releaseClose() {
        std::lock_guard<std::mutex> lk(mtx);
        closeReleased = true;
        cv.notify_all();
    }
};

std::atomic<Schedule*> current{nullptr};
std::atomic<wavSinkTest::Preparation> preparationFailure{wavSinkTest::Preparation::None};

struct OpenGate {
    std::mutex mtx;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
};

class ScheduledOutput : public WavSinkOutput {
public:
    explicit ScheduledOutput(Schedule& s) : schedule_(s) {}
    Result open(const std::wstring& path, const AudioFormat& fmt) override {
        return writer_.open(path, fmt);
    }
    Result setBuffer(size_t bytes) override {
        return writer_.setStdioBuffer(bytes);
    }
    Result write(const void* data, size_t bytes) override { return writer_.write(data, bytes); }
    Result close() override {
        {
            std::unique_lock<std::mutex> lk(schedule_.mtx);
            schedule_.closeEntered = true;
            schedule_.cv.notify_all();
            schedule_.cv.wait(lk, [&] { return schedule_.closeReleased; });
        }
        return writer_.close();
    }
private:
    Schedule& schedule_;
    WavWriter writer_;
};

class FailingOutput : public WavSinkOutput {
public:
    explicit FailingOutput(OpenGate* writeGate = nullptr) : writeGate_(writeGate) {}
    Result open(const std::wstring& path, const AudioFormat& fmt) override {
        return writer_.open(path, fmt);
    }
    Result setBuffer(size_t bytes) override {
        return writer_.setStdioBuffer(bytes);
    }
    Result write(const void*, size_t) override {
        if (writeGate_) {
            std::unique_lock<std::mutex> lk(writeGate_->mtx);
            writeGate_->cv.wait(lk, [&] { return writeGate_->released; });
        }
        return Result::Fail(101, "first write failure");
    }
    Result close() override {
        writer_.close();
        return Result::Fail(102, "later close failure");
    }
private:
    OpenGate* writeGate_;
    WavWriter writer_;
};

class ThrowingBufferOutput : public WavSinkOutput {
public:
    explicit ThrowingBufferOutput(bool& closed) : closed_(closed) {}
    Result open(const std::wstring& path, const AudioFormat& fmt) override {
        return writer_.open(path, fmt);
    }
    Result setBuffer(size_t) override { throw std::bad_alloc(); }
    Result write(const void*, size_t) override { return Result::Ok(); }
    Result close() override { closed_ = true; return writer_.close(); }
private:
    bool& closed_;
    WavWriter writer_;
};

class SlowOpenOutput : public WavSinkOutput {
public:
    explicit SlowOpenOutput(OpenGate& gate) : gate_(gate) {}
    Result open(const std::wstring& path, const AudioFormat& fmt) override {
        std::unique_lock<std::mutex> lk(gate_.mtx);
        gate_.entered = true;
        gate_.cv.notify_all();
        gate_.cv.wait(lk, [&] { return gate_.released; });
        return writer_.open(path, fmt);
    }
    Result setBuffer(size_t bytes) override { return writer_.setStdioBuffer(bytes); }
    Result write(const void* data, size_t bytes) override { return writer_.write(data, bytes); }
    Result close() override { return writer_.close(); }
private:
    OpenGate& gate_;
    WavWriter writer_;
};

std::wstring tempPath(const wchar_t* tag) {
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    static unsigned serial = 0;
    return std::wstring(dir) + L"wa_lifetime_" + tag + L"_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++serial) + L".wav";
}

void expectPcm(const std::wstring& path, const std::vector<int16_t>& expected) {
    WavReader reader;
    ASSERT_TRUE(reader.open(path));
    EXPECT_TRUE(reader.format() == (AudioFormat{48000, 2, 16, false}));
    std::vector<int16_t> back(expected.size());
    EXPECT_EQ(reader.read(back.data(), back.size() * sizeof(int16_t)),
              back.size() * sizeof(int16_t));
    EXPECT_EQ(back, expected);
    uint8_t extra = 0;
    EXPECT_EQ(reader.read(&extra, 1), 0u);
    reader.close();
    DeleteFileW(path.c_str());
}
} // namespace

namespace wa::wavSinkTest {
void prepare(Preparation operation) {
    if (preparationFailure.load() != operation) return;
    if (operation == Preparation::Thread)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    throw std::bad_alloc();
}

void checkpoint(Point point, const WavSink* sink) {
    Schedule* s = current;
    if (!s) return;
    std::unique_lock<std::mutex> lk(s->mtx);
    if (point == Point::AfterAdmission && sink == s->sink)
        s->producer = std::this_thread::get_id();
    if (point != s->point || s->released ||
        (point == Point::BeforePublish ? s->producer != std::this_thread::get_id()
                                      : sink != s->sink)) return;
    s->paused = true;
    s->cv.notify_all();
    s->cv.wait(lk, [&] { return s->released; });
}

void drainObserved(const WavSink* sink, bool active) {
    Schedule* s = current;
    if (!s || sink != s->sink) return;
    std::lock_guard<std::mutex> lk(s->mtx);
    s->drainSeen = true;
    s->drainActive = active;
    s->cv.notify_all();
}
} // namespace wa::wavSinkTest

static void stopWhilePaused(Point point) {
    Schedule s;
    s.point = point;
    WavSink sink([&] { return std::make_unique<ScheduledOutput>(s); });
    const std::wstring path = tempPath(L"admitted");
    ASSERT_TRUE(sink.startExact(path, AudioFormat{48000, 2, 16, false}));
    s.sink = &sink;
    current = &s;
    const std::vector<int16_t> pcm{11, -22, 33, -44};
    size_t accepted = 0;
    std::thread producer([&] { accepted = sink.push(pcm.data(), pcm.size() * 2); });
    EXPECT_TRUE(s.wait([&] { return s.paused; })) << "producer deadlock";
    Result result;
    std::thread stopper([&] { result = sink.stop(); });
    const bool observed = s.wait([&] { return s.drainSeen && (s.drainActive || s.closeEntered); });
    EXPECT_TRUE(observed) << "writer deadlock";
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        EXPECT_FALSE(s.closeEntered) << "file closed while an admitted push was paused";
    }
    s.releaseProducer();
    producer.join();
    s.releaseClose();
    stopper.join();
    current = nullptr;
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(accepted, pcm.size() * 2);
    expectPcm(path, pcm);
}

TEST(WavSinkLifecycle, StopWaitsForAdmittedPushAndKeepsItsBytes) {
    stopWhilePaused(Point::AfterAdmission);
}

TEST(WavSinkLifecycle, EmptyRingDoesNotFinalizeBeforeLastPublication) {
    stopWhilePaused(Point::BeforePublish);
}

TEST(WavSinkLifecycle, OldSnapshotCannotWriteIntoRestartedFile) {
    WavSink sink;
    const AudioFormat fmt{48000, 2, 16, false};
    const auto oldPath = tempPath(L"old");
    const auto newPath = tempPath(L"new");
    ASSERT_TRUE(sink.startExact(oldPath, fmt));
    Schedule s;
    s.sink = &sink;
    s.point = Point::AfterSnapshot;
    current = &s;
    const std::vector<int16_t> stale{111, -111};
    size_t accepted = 0;
    std::thread producer([&] { accepted = sink.push(stale.data(), stale.size() * 2); });
    EXPECT_TRUE(s.wait([&] { return s.paused; })) << "producer deadlock";
    EXPECT_TRUE(sink.stop());
    EXPECT_TRUE(sink.startExact(newPath, fmt));
    s.releaseProducer();
    producer.join();
    current = nullptr;
    const std::vector<int16_t> fresh{222, -222, 333, -333};
    EXPECT_EQ(sink.push(fresh.data(), fresh.size() * 2), fresh.size() * 2);
    EXPECT_TRUE(sink.stop());
    EXPECT_EQ(accepted, 0u);
    expectPcm(oldPath, {});
    expectPcm(newPath, fresh);
}

TEST(WavSinkLifecycle, WriteFailureSurvivesCloseFailureAndRepeatedStop) {
    WavSink sink([] { return std::make_unique<FailingOutput>(); });
    const auto path = tempPath(L"first_error");
    ASSERT_TRUE(sink.startExact(path, AudioFormat{48000, 2, 16, false}));
    const int16_t pcm[2]{11, -11};
    EXPECT_EQ(sink.push(pcm, sizeof(pcm)), sizeof(pcm));
    const Result first = sink.stop();
    EXPECT_FALSE(first);
    EXPECT_EQ(first.message, "first write failure");
    const Result repeated = sink.stop();
    EXPECT_FALSE(repeated);
    EXPECT_EQ(repeated.message, first.message);
    DeleteFileW(path.c_str());
}

TEST(WavSinkLifecycle, PreparationExceptionClosesOutputAndAllowsRetry) {
    bool closed = false;
    bool fail = true;
    WavSink sink([&]() -> std::unique_ptr<WavSinkOutput> {
        if (fail) return std::make_unique<ThrowingBufferOutput>(closed);
        return std::make_unique<ScheduledOutput>(*current.load());
    });
    const auto path = tempPath(L"prepare_exception");
    EXPECT_FALSE(sink.startExact(path, AudioFormat{48000, 2, 16, false}));
    EXPECT_TRUE(closed);
    EXPECT_FALSE(sink.stop());
    fail = false;
    Schedule s;
    s.closeReleased = true;
    current = &s;
    EXPECT_TRUE(sink.startExact(path, AudioFormat{48000, 2, 16, false}));
    EXPECT_TRUE(sink.stop());
    current = nullptr;
    DeleteFileW(path.c_str());
}

TEST(WavSinkLifecycle, DifferentSourceCannotWriteIntoCurrentFile) {
    WavSink sink;
    const auto path = tempPath(L"source");
    ASSERT_TRUE(sink.startExact(path, AudioFormat{48000, 2, 16, false}, 42));
    const std::vector<int16_t> wrong{111, -111};
    const std::vector<int16_t> right{222, -222};
    EXPECT_EQ(sink.push(wrong.data(), wrong.size() * 2, 41), 0u);
    EXPECT_EQ(sink.push(right.data(), right.size() * 2, 42), right.size() * 2);
    EXPECT_TRUE(sink.stop());
    expectPcm(path, right);
}

TEST(WavSinkLifecycle, OverflowRemainsFirstFailureWhenOutputAlsoFails) {
    OpenGate gate;
    WavSink sink([&] { return std::make_unique<FailingOutput>(&gate); });
    const auto path = tempPath(L"overflow_first");
    ASSERT_TRUE(sink.startExact(path, AudioFormat{48000, 2, 16, false}));
    std::vector<uint8_t> blob(2u * 1024u * 1024u, 0x11);
    EXPECT_LT(sink.push(blob.data(), blob.size()), blob.size());
    EXPECT_TRUE(sink.hasFailed());
    {
        std::lock_guard<std::mutex> lk(gate.mtx);
        gate.released = true;
        gate.cv.notify_all();
    }
    const Result result = sink.stop();
    EXPECT_FALSE(result);
    EXPECT_EQ(result.message, "WavSink: ring overflow");
    EXPECT_EQ(sink.stop().message, result.message);
    DeleteFileW(path.c_str());
}

TEST(WavSinkLifecycle, StopRequestDuringPreparationCannotPublishReady) {
    OpenGate gate;
    WavSink sink([&] { return std::make_unique<SlowOpenOutput>(gate); });
    const auto path = tempPath(L"cancel_preparing");
    Result started;
    std::thread starter([&] { started = sink.startExact(path, AudioFormat{48000, 2, 16, false}, 42); });
    {
        std::unique_lock<std::mutex> lk(gate.mtx);
        EXPECT_TRUE(gate.cv.wait_for(lk, kDeadlockTimeout, [&] { return gate.entered; }))
            << "open deadlock";
    }
    sink.requestStop(42);
    {
        std::lock_guard<std::mutex> lk(gate.mtx);
        gate.released = true;
        gate.cv.notify_all();
    }
    starter.join();
    EXPECT_FALSE(started);
    EXPECT_FALSE(sink.isRunning());
    EXPECT_FALSE(sink.hasFailed()) << "normal cancellation is not a file failure";
    EXPECT_TRUE(sink.stop());
    expectPcm(path, {});
}

TEST(WavSinkLifecycle, AllocationAndThreadLaunchFailuresReleaseResourcesForRetry) {
    for (const auto operation : {wavSinkTest::Preparation::Ring, wavSinkTest::Preparation::Scratch,
                                  wavSinkTest::Preparation::Thread}) {
        WavSink sink;
        const auto path = tempPath(L"prepare_retry");
        preparationFailure = operation;
        EXPECT_FALSE(sink.startExact(path, AudioFormat{48000, 2, 16, false}));
        EXPECT_TRUE(sink.hasFailed());
        EXPECT_FALSE(sink.stop());
        preparationFailure = wavSinkTest::Preparation::None;
        EXPECT_TRUE(sink.startExact(path, AudioFormat{48000, 2, 16, false}));
        EXPECT_FALSE(sink.hasFailed());
        EXPECT_TRUE(sink.stop());
        expectPcm(path, {});
    }
}

TEST(WavSinkLifecycle, NormalRejectionAndWrongSourceStopAreNotFileFailures) {
    WavSink sink;
    const auto path = tempPath(L"normal_rejection");
    ASSERT_TRUE(sink.startExact(path, AudioFormat{48000, 2, 16, false}, 42));
    sink.requestStop(41);
    const std::vector<int16_t> pcm{11, -11};
    EXPECT_EQ(sink.push(pcm.data(), pcm.size() * 2, 42), pcm.size() * 2);
    sink.requestStop(42);
    EXPECT_EQ(sink.push(pcm.data(), pcm.size() * 2, 42), 0u);
    EXPECT_FALSE(sink.hasFailed());
    EXPECT_TRUE(sink.stop());
    EXPECT_TRUE(sink.stop());
    expectPcm(path, pcm);
}

TEST(WavSinkLifecycle, OverflowAndStopDrainAcceptedPrefixAfterPublication) {
    Schedule s;
    s.point = Point::BeforePublish;
    WavSink sink([&] { return std::make_unique<ScheduledOutput>(s); });
    const auto path = tempPath(L"overflow_stop");
    ASSERT_TRUE(sink.startExact(path, AudioFormat{48000, 2, 16, false}));
    s.sink = &sink;
    current = &s;
    std::vector<int16_t> pcm(1024u * 1024u, 123);
    size_t accepted = 0;
    std::thread producer([&] { accepted = sink.push(pcm.data(), pcm.size() * 2); });
    EXPECT_TRUE(s.wait([&] { return s.paused; })) << "producer deadlock";
    Result stopped;
    std::thread stopper([&] { stopped = sink.stop(); });
    EXPECT_TRUE(s.wait([&] { return s.drainSeen && s.drainActive; })) << "writer deadlock";
    s.releaseProducer();
    producer.join();
    s.releaseClose();
    stopper.join();
    current = nullptr;
    EXPECT_LT(accepted, pcm.size() * 2);
    EXPECT_FALSE(stopped);
    EXPECT_EQ(stopped.message, "WavSink: ring overflow");
    EXPECT_EQ(sink.stop().message, stopped.message);
    pcm.resize(accepted / 2);
    expectPcm(path, pcm);
}
