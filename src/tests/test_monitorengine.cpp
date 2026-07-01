// Fake-backend unit tests for wa::MonitorEngine. No WASAPI hardware is touched:
// a fake IAudioBackend captures the RingBuffer it is opened with, exposes pushPcm()
// to inject bytes + signal the pump's wake event, and returns a chosen AudioFormat
// from stats(). This drives the entire pump (prefill, rate-fail, rollback, scope,
// xrun) deterministically.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include "MonitorEngine.h"
#include "RingBuffer.h"

using namespace wa;

namespace {

// ---------------------------------------------------------------------------
// Fake backend: the test seam.
// ---------------------------------------------------------------------------
class FakeBackend : public IAudioBackend {
public:
    FakeBackend(AudioFormat fmt, bool failStart, std::atomic<bool>* stoppedOut)
        : fmt_(fmt), failStart_(failStart), stoppedOut_(stoppedOut) {
        evt_ = CreateEventW(nullptr, /*manualReset*/ FALSE, /*initial*/ FALSE, nullptr);
    }
    ~FakeBackend() override {
        if (evt_) CloseHandle(evt_);
    }

    Result open(const DeviceId&, const AudioFormat&, RingBuffer* ring) override {
        ring_ = ring;
        return Result::Ok();
    }
    Result start() override {
        if (failStart_) return Result::Fail(123, "fake: start failed");
        started_.store(true, std::memory_order_relaxed);
        return Result::Ok();
    }
    void stop() override {
        started_.store(false, std::memory_order_relaxed);
        if (stoppedOut_) stoppedOut_->store(true, std::memory_order_relaxed);
    }
    void close() override {}
    BackendStats stats() const override {
        BackendStats s{};
        s.actualFormat = fmt_;
        s.bufferFrames = bufferFrames_;
        return s;
    }
    void* dataReadyEvent() const override { return evt_; }

    // Test driver: push raw interleaved PCM bytes into the capture ring + wake pump.
    void pushPcm(const void* data, size_t bytes) {
        if (ring_) ring_->write(data, bytes);
        if (evt_) SetEvent(evt_);
    }

    AudioFormat       fmt_;
    bool              failStart_   = false;
    std::atomic<bool>*stoppedOut_  = nullptr;
    std::atomic<bool> started_{false};
    uint32_t          bufferFrames_ = 0;
    RingBuffer*       ring_ = nullptr;
    HANDLE            evt_  = nullptr;
};

// Owns the cross-thread observable state and hands MonitorEngine a factory. The
// engine owns the fake objects; capPtr/renderPtr stay valid only while it does.
struct FakeRig {
    AudioFormat capFmt{48000, 2, 16, false};
    AudioFormat renderFmt{48000, 2, 16, false};
    bool        renderFailStart = false;

    std::atomic<bool> capStopped{false};
    std::atomic<bool> renderStopped{false};
    FakeBackend*      capPtr    = nullptr;
    FakeBackend*      renderPtr = nullptr;

    MonitorEngine::BackendFactory factory() {
        return [this](DataFlow flow) -> std::unique_ptr<IAudioBackend> {
            if (flow == DataFlow::Capture) {
                auto b  = std::make_unique<FakeBackend>(capFmt, false, &capStopped);
                capPtr  = b.get();
                return b;
            }
            auto b    = std::make_unique<FakeBackend>(renderFmt, renderFailStart, &renderStopped);
            renderPtr = b.get();
            return b;
        };
    }
};

// Build interleaved int16 PCM where every channel of frame i holds value[i].
std::vector<uint8_t> makeRampPcm(const std::vector<int16_t>& values, uint16_t channels) {
    std::vector<uint8_t> bytes(values.size() * channels * sizeof(int16_t));
    auto* out = reinterpret_cast<int16_t*>(bytes.data());
    for (size_t i = 0; i < values.size(); ++i)
        for (uint16_t c = 0; c < channels; ++c)
            out[i * channels + c] = values[i];
    return bytes;
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(MonitorEngine, RateMismatchFails) {
    FakeRig rig;
    rig.capFmt    = {48000, 2, 16, false};
    rig.renderFmt = {44100, 2, 16, false};

    MonitorEngine eng(rig.factory());
    Result r = eng.start(BackendKind::WasapiShared, L"", L"", 50);

    EXPECT_FALSE(static_cast<bool>(r));
    MonitorStatus st = eng.poll();
    EXPECT_NE(st.overall, StreamState::Running);
    EXPECT_EQ(st.overall, StreamState::Error);
    EXPECT_EQ(st.errorCode, static_cast<uint32_t>(MonitorError::RateMismatch));
    // Clean rollback: both fakes were stopped.
    EXPECT_TRUE(rig.capStopped.load());
    EXPECT_TRUE(rig.renderStopped.load());
}

TEST(MonitorEngine, PrefillThenRunning) {
    FakeRig rig; // both 48000/2/16 by default
    MonitorEngine eng(rig.factory());

    const uint32_t delayMs = 50;
    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", delayMs)));

    // Before any capture data: not Running yet, and renderXruns not counted.
    MonitorStatus st0 = eng.poll();
    EXPECT_NE(st0.overall, StreamState::Running);
    EXPECT_EQ(st0.renderXruns, 0u);
    ASSERT_NE(rig.capPtr, nullptr);

    // Push more than (delay + one render period) of stereo frames to cross prefill.
    const uint32_t frames = 4000; // 50ms@48k = 2400 frames; +period(480) => prefill 2880
    std::vector<int16_t> ramp(frames);
    for (uint32_t i = 0; i < frames; ++i) ramp[i] = static_cast<int16_t>(i % 1000);
    auto pcm = makeRampPcm(ramp, rig.capFmt.channels);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] { return eng.poll().overall == StreamState::Running; }))
        << "engine never reached Running after prefill";

    MonitorStatus st1 = eng.poll();
    EXPECT_EQ(st1.overall, StreamState::Running);
    EXPECT_EQ(st1.renderXruns, 0u) << "prefill must not inflate renderXruns";
    EXPECT_EQ(st1.sampleRate, 48000u);
    EXPECT_EQ(st1.delayMs, delayMs);

    // A second push (now Running) must drive the render tap without overrunning.
    rig.capPtr->pushPcm(pcm.data(), pcm.size());
    EXPECT_TRUE(waitFor([&] { return eng.renderWritten() > 0; }))
        << "render scope never advanced after Running";
    EXPECT_EQ(eng.poll().renderXruns, 0u);

    eng.stop();
}

TEST(MonitorEngine, CaptureScopePopulated) {
    FakeRig rig; // stereo 48000/2/16; both channels carry the same ramp value
    MonitorEngine eng(rig.factory());
    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", 20)));
    ASSERT_NE(rig.capPtr, nullptr);

    const uint32_t frames = 256;
    std::vector<int16_t> ramp(frames);
    for (uint32_t i = 0; i < frames; ++i) ramp[i] = static_cast<int16_t>(i * 100); // <= 25500
    auto pcm = makeRampPcm(ramp, rig.capFmt.channels);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] { return eng.capWritten() >= frames; }))
        << "capWritten never reached " << frames;
    EXPECT_EQ(eng.capWritten(), static_cast<uint64_t>(frames));

    float out[64];
    uint64_t endIdx = 0;
    ASSERT_TRUE(eng.snapshotCapture(64, out, endIdx));
    EXPECT_EQ(endIdx, static_cast<uint64_t>(frames));
    // Downmix of (v,v) stereo is v; pcmToFloat scales int16 by 1/32768.
    for (uint32_t k = 0; k < 64; ++k) {
        const float expected = static_cast<float>(ramp[frames - 64 + k]) / 32768.f;
        EXPECT_NEAR(out[k], expected, 1e-4f) << "mismatch at k=" << k;
    }

    eng.stop();
}

TEST(MonitorEngine, StartRollbackOnRenderFail) {
    FakeRig rig;
    rig.renderFailStart = true; // render backend's start() returns Fail

    MonitorEngine eng(rig.factory());
    Result r = eng.start(BackendKind::WasapiShared, L"", L"", 50);

    EXPECT_FALSE(static_cast<bool>(r));
    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.overall, StreamState::Idle); // clean rollback to Idle
    EXPECT_EQ(st.errorCode, static_cast<uint32_t>(MonitorError::RenderStart));
    EXPECT_TRUE(rig.capStopped.load()) << "capture backend must be stopped on rollback";
}

TEST(MonitorEngine, StopJoinsCleanly) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", 50)));
    ASSERT_NE(rig.capPtr, nullptr);

    // Feed a little so the pump actually runs, then stop and time the join.
    std::vector<int16_t> ramp(1024, 7);
    auto pcm = makeRampPcm(ramp, rig.capFmt.channels);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());
    waitFor([&] { return eng.capWritten() >= 1024; });

    const auto t0 = std::chrono::steady_clock::now();
    eng.stop();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_LT(elapsedMs, 2000) << "stop() did not return promptly (possible pump-join hang)";

    EXPECT_EQ(eng.poll().overall, StreamState::Idle);

    // Second stop is a safe no-op.
    eng.stop();
    EXPECT_EQ(eng.poll().overall, StreamState::Idle);
}

TEST(MonitorEngine, InvalidDelayRejected) {
    // delayMs = 999999 vastly exceeds the 10-s cap; must return Fail without
    // throwing (core no-throw-across-public-API contract).
    FakeRig rig; // matching cap/render rates (48000/2/16 both)
    MonitorEngine eng(rig.factory());

    Result r = eng.start(BackendKind::WasapiShared, L"", L"", 999999u);

    EXPECT_FALSE(static_cast<bool>(r));
    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.overall, StreamState::Error);
    EXPECT_EQ(st.errorCode, static_cast<uint32_t>(MonitorError::InvalidDelay));
}
