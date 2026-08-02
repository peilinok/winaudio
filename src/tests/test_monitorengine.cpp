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
    FakeBackend(AudioFormat fmt, bool failStart, std::atomic<bool>* stoppedOut,
                std::atomic<int>* openDoneOut = nullptr, bool failOpen = false)
        : fmt_(fmt), failStart_(failStart), stoppedOut_(stoppedOut),
          openDoneOut_(openDoneOut), failOpen_(failOpen) {
        evt_ = CreateEventW(nullptr, /*manualReset*/ FALSE, /*initial*/ FALSE, nullptr);
    }
    ~FakeBackend() override {
        if (evt_) CloseHandle(evt_);
    }

    Result open(const DeviceId&, const AudioFormat&, RingBuffer* ring,
                const StreamParams& params) override {
        if (failOpen_) return Result::Fail(122, "fake: open failed");
        ring_ = ring;
        lastOpenParams_ = params;
        if (openDoneOut_) openDoneOut_->fetch_add(1, std::memory_order_release);
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
        s.idleSilenceFrames = idleSilenceFrames_;
        s.silentPacketFrames = silentPacketFrames_;
        return s;
    }
    void* dataReadyEvent() const override { return evt_; }

    // Test driver: push raw interleaved PCM bytes into the capture ring + wake pump.
    void pushPcm(const void* data, size_t bytes) {
        if (ring_) ring_->write(data, bytes);
        if (evt_) SetEvent(evt_);
    }

    AudioFormat        fmt_;
    bool               failStart_   = false;
    std::atomic<bool>* stoppedOut_  = nullptr;
    std::atomic<int>*  openDoneOut_ = nullptr;
    bool               failOpen_    = false;
    std::atomic<bool>  started_{false};
    uint32_t          bufferFrames_ = 0;
    uint64_t          idleSilenceFrames_ = 0;
    uint64_t          silentPacketFrames_ = 0;
    StreamParams      lastOpenParams_{};
    RingBuffer*       ring_ = nullptr;
    HANDLE            evt_  = nullptr;
    AudioFormat       lastRequested_{};
    bool              sawRequested_ = false;
};

// Owns the cross-thread observable state and hands MonitorEngine a factory. The
// engine owns the fake objects; capPtr/renderPtr stay valid only while it does.
struct FakeRig {
    AudioFormat capFmt{48000, 2, 16, false};
    AudioFormat renderFmt{48000, 2, 16, false};
    bool        renderFailStart = false;
    bool        silentFailOpen = false;
    bool        silentFailStart = false;

    std::atomic<bool> capStopped{false};
    std::atomic<bool> renderStopped{false};
    std::atomic<bool> silentStopped{false};
    std::atomic<int>  capOpenCount{0};
    std::atomic<int>  renderOpenCount{0};  // increments each time render factory is called
    std::atomic<int>  silentOpenCount{0};
    std::atomic<int>  renderOpenDone{0};   // release-incremented inside open() for race-free sync
    FakeBackend*      capPtr    = nullptr;
    FakeBackend*      renderPtr = nullptr;
    FakeBackend*      silentPtr = nullptr;
    CaptureSource     lastCaptureSource{};
    bool              sawCaptureSource = false;

    MonitorEngine::BackendFactory factory() {
        return [this](DataFlow flow, const CaptureSource* source,
                      const AudioFormat* req) -> std::unique_ptr<IAudioBackend> {
            if (flow == DataFlow::Capture) {
                capOpenCount.fetch_add(1, std::memory_order_relaxed);
                if (source) { lastCaptureSource = *source; sawCaptureSource = true; }
                auto b  = std::make_unique<FakeBackend>(capFmt, false, &capStopped);
                capPtr  = b.get();
                if (req) { b->lastRequested_ = *req; b->sawRequested_ = true; }
                return b;
            }
            renderOpenCount.fetch_add(1, std::memory_order_relaxed);
            renderStopped.store(false, std::memory_order_relaxed); // reset for each new backend
            auto b    = std::make_unique<FakeBackend>(renderFmt, renderFailStart, &renderStopped, &renderOpenDone);
            renderPtr = b.get();
            return b;
        };
    }

    MonitorEngine::SilentRenderFactory silentFactory() {
        return [this](const AudioFormat* req) -> std::unique_ptr<IAudioBackend> {
            silentOpenCount.fetch_add(1, std::memory_order_relaxed);
            auto b = std::make_unique<FakeBackend>(renderFmt, silentFailStart, &silentStopped,
                                                   nullptr, silentFailOpen);
            silentPtr = b.get();
            if (req) { b->lastRequested_ = *req; b->sawRequested_ = true; }
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

std::vector<uint8_t> makeStereoPcm(const std::vector<int16_t>& left,
                                   const std::vector<int16_t>& right) {
    std::vector<uint8_t> bytes(left.size() * 2u * sizeof(int16_t));
    auto* out = reinterpret_cast<int16_t*>(bytes.data());
    for (size_t i = 0; i < left.size(); ++i) {
        out[i * 2u] = left[i];
        out[i * 2u + 1u] = right[i];
    }
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
    EXPECT_EQ(st.capState, StreamState::Idle); // capture rolled back
    // Clean rollback: both fakes were stopped.
    EXPECT_TRUE(rig.capStopped.load());
    EXPECT_TRUE(rig.renderStopped.load());
}

TEST(MonitorEngine, PrefillThenRunning) {
    FakeRig rig; // both 48000/2/16 by default
    MonitorEngine eng(rig.factory());

    const uint32_t delayMs = 50;
    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", delayMs)));

    // overall is Running immediately after capture starts (not gated on prefill anymore).
    // During the render prefill phase renderXruns must not be inflated.
    MonitorStatus st0 = eng.poll();
    EXPECT_EQ(st0.overall, StreamState::Running);
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

TEST(MonitorEngine, CaptureChannelSnapshotsExposeActualChannels) {
    FakeRig rig;
    rig.capFmt = {48000, 2, 16, false};
    MonitorEngine eng(rig.factory());
    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", 20,
                                            false)));
    ASSERT_NE(rig.capPtr, nullptr);

    const uint32_t frames = 256;
    std::vector<int16_t> left(frames);
    std::vector<int16_t> right(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        left[i] = static_cast<int16_t>(100 + i);
        right[i] = static_cast<int16_t>(1000 + i);
    }
    auto pcm = makeStereoPcm(left, right);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] { return eng.capWritten() >= frames; }))
        << "capWritten never reached " << frames;
    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.captureChannels, 2u);

    float leftOut[8] = {};
    float rightOut[8] = {};
    float monoOut[8] = {};
    uint64_t leftEnd = 0;
    uint64_t rightEnd = 0;
    uint64_t monoEnd = 0;
    ASSERT_TRUE(eng.snapshotCaptureChannel(0, 8, leftOut, leftEnd));
    ASSERT_TRUE(eng.snapshotCaptureChannel(1, 8, rightOut, rightEnd));
    ASSERT_TRUE(eng.snapshotCapture(8, monoOut, monoEnd));
    EXPECT_EQ(leftEnd, static_cast<uint64_t>(frames));
    EXPECT_EQ(rightEnd, static_cast<uint64_t>(frames));
    EXPECT_EQ(monoEnd, static_cast<uint64_t>(frames));

    for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t idx = frames - 8 + k;
        const float expectedLeft = static_cast<float>(left[idx]) / 32768.f;
        const float expectedRight = static_cast<float>(right[idx]) / 32768.f;
        EXPECT_NEAR(leftOut[k], expectedLeft, 1e-4f) << "left mismatch at k=" << k;
        EXPECT_NEAR(rightOut[k], expectedRight, 1e-4f) << "right mismatch at k=" << k;
        EXPECT_NEAR(monoOut[k], (expectedLeft + expectedRight) * 0.5f, 1e-4f)
            << "mono mismatch at k=" << k;
    }

    uint64_t end = 0;
    EXPECT_FALSE(eng.snapshotCaptureChannel(2, 8, leftOut, end));

    eng.stop();
}

TEST(MonitorEngine, CaptureChannelSnapshotAtUsesRequestedEndIndex) {
    FakeRig rig;
    rig.capFmt = {48000, 2, 16, false};
    MonitorEngine eng(rig.factory());
    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", 20,
                                            false)));
    ASSERT_NE(rig.capPtr, nullptr);

    const uint32_t frames = 256;
    std::vector<int16_t> left(frames);
    std::vector<int16_t> right(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        left[i] = static_cast<int16_t>(100 + i);
        right[i] = static_cast<int16_t>(1000 + i);
    }
    auto pcm = makeStereoPcm(left, right);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] { return eng.capWritten() >= frames; }))
        << "capWritten never reached " << frames;

    float out[8] = {};
    ASSERT_TRUE(eng.snapshotCaptureChannelAt(1, 128, 8, out));
    for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t idx = 120 + k;
        const float expected = static_cast<float>(right[idx]) / 32768.f;
        EXPECT_NEAR(out[k], expected, 1e-4f) << "right mismatch at k=" << k;
    }

    EXPECT_FALSE(eng.snapshotCaptureChannelAt(1, frames + 1, 8, out));

    eng.stop();
}

TEST(MonitorEngine, StartRollbackOnRenderFail) {
    FakeRig rig;
    rig.renderFailStart = true; // render backend's start() returns Fail

    MonitorEngine eng(rig.factory());
    Result r = eng.start(BackendKind::WasapiShared, L"", L"", 50);

    EXPECT_FALSE(static_cast<bool>(r));
    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.overall, StreamState::Error); // engage failure -> Error (capture rolled back)
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

// ---------------------------------------------------------------------------
// Task 1 new tests: runtime playback toggle
// ---------------------------------------------------------------------------

TEST(MonitorEngine, PlaybackStartsDisabled) {
    FakeRig rig; // 48000/2/16 both
    MonitorEngine eng(rig.factory());

    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", 50, false)));

    // Render factory must NOT have been called.
    EXPECT_EQ(rig.renderOpenCount.load(), 0);

    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.capState, StreamState::Running);
    EXPECT_EQ(st.overall, StreamState::Running);
    EXPECT_EQ(st.renderState, StreamState::Idle);

    eng.stop();
}

TEST(MonitorEngine, EnablePlaybackEngagesRender) {
    FakeRig rig; // 48000/2/16 both
    MonitorEngine eng(rig.factory());

    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", 50, false)));
    ASSERT_EQ(rig.renderOpenCount.load(), 0); // no render at start

    // Enable playback and wake the pump with capture data.
    eng.setPlaybackEnabled(true);

    std::vector<int16_t> ramp(1024, 42);
    auto pcm = makeRampPcm(ramp, rig.capFmt.channels);
    ASSERT_NE(rig.capPtr, nullptr);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] { return eng.poll().renderState == StreamState::Running; }))
        << "renderState never reached Running after setPlaybackEnabled(true)";
    EXPECT_GT(rig.renderOpenCount.load(), 0) << "render factory must have been called";
    EXPECT_EQ(eng.poll().overall, StreamState::Running) << "capture must still be running";

    eng.stop();
}

TEST(MonitorEngine, DisablePlaybackStopsRender) {
    FakeRig rig; // 48000/2/16 both
    MonitorEngine eng(rig.factory());

    // Start with playback enabled; render is engaged synchronously in start().
    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", 50, true)));
    // renderState is set to Running in engageRender() before pump launch.
    ASSERT_EQ(eng.poll().renderState, StreamState::Running);

    ASSERT_NE(rig.capPtr, nullptr);

    // Push enough frames to cross prefill (delay=50ms@48k=2400 + period(sr/100=480) = 2880)
    // then confirm render scope advances, proving data has actually flowed to renderScope_.
    const uint32_t bigBatch = 5000; // exceeds prefillFrames_ comfortably
    std::vector<int16_t> ramp(bigBatch, 7);
    auto pcm = makeRampPcm(ramp, rig.capFmt.channels);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] { return eng.renderWritten() > 0; }))
        << "renderWritten never advanced; prefill did not complete before disengage";

    // Capture the monotonic baseline for the anti-UAF invariant check below.
    const uint64_t wBefore = eng.renderWritten();
    ASSERT_GT(wBefore, 0u);

    eng.setPlaybackEnabled(false);
    // Wake pump so it observes wantPlayback_=false and calls disengageRender().
    std::vector<int16_t> ramp2(1024, 7);
    auto pcm2 = makeRampPcm(ramp2, rig.capFmt.channels);
    rig.capPtr->pushPcm(pcm2.data(), pcm2.size());

    ASSERT_TRUE(waitFor([&] { return eng.poll().renderState == StreamState::Idle; }))
        << "renderState never reached Idle after setPlaybackEnabled(false)";
    EXPECT_TRUE(rig.renderStopped.load()) << "render stop() must have been called on disengage";

    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.overall, StreamState::Running) << "capture must still be running after disengage";

    // Anti-UAF invariant (invariant-1): renderScope_ is session-lifetime and is NEVER
    // reset or reallocated inside disengageRender(). If it were wrongly reset, totalWritten()
    // would fall back to 0, making this assertion fail and catching the regression.
    EXPECT_GE(eng.renderWritten(), wBefore)
        << "renderScope_ must be monotonic across disengage (anti-UAF invariant-1)";

    // snapshotRender must not crash: renderScope_ is session-lifetime even when disengaged.
    float buf[16] = {};
    uint64_t endIdx = 0;
    eng.snapshotRender(16, buf, endIdx); // must not crash (returns false/stale when idle)

    eng.stop();
}

TEST(MonitorEngine, EnablePlaybackRateMismatch) {
    FakeRig rig;
    rig.capFmt    = {48000, 2, 16, false};
    rig.renderFmt = {44100, 2, 16, false}; // mismatch

    MonitorEngine eng(rig.factory());

    // Start with playback disabled (skips synchronous engage -> capture succeeds).
    ASSERT_TRUE(static_cast<bool>(eng.start(BackendKind::WasapiShared, L"", L"", 50, false)));
    EXPECT_EQ(eng.poll().capState, StreamState::Running);

    // Enable playback; pump will try engageRender() and detect rate mismatch.
    eng.setPlaybackEnabled(true);

    ASSERT_NE(rig.capPtr, nullptr);
    std::vector<int16_t> ramp(1024, 3);
    auto pcm = makeRampPcm(ramp, rig.capFmt.channels);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] {
        MonitorStatus s = eng.poll();
        return s.renderState == StreamState::Error || s.renderState == StreamState::Running;
    })) << "pump never settled render state after mismatch";

    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.renderState, StreamState::Error);
    EXPECT_EQ(st.errorCode, static_cast<uint32_t>(MonitorError::RateMismatch));
    EXPECT_EQ(st.capState, StreamState::Running) << "capture must continue after render engage failure";
    EXPECT_EQ(st.overall, StreamState::Running);

    eng.stop();
}

// ---------------------------------------------------------------------------
// Task 3 new tests: StreamParams plumbed through MonitorEngine
// ---------------------------------------------------------------------------

TEST(MonitorEngine, StreamParamsReachBackends) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    StreamParams cap; cap.bufferMs = 30;
    cap.clientProperties.enabled = true;
    cap.clientProperties.category = AudioCategory::Speech;
    cap.clientProperties.offload = true;
    cap.clientProperties.option = StreamOption::Ambisonics;
    StreamParams ren; ren.clientProperties.enabled = true;
    ren.clientProperties.category = AudioCategory::Media;
    ren.ducking = DuckingMode::OptOut;
    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, L"", L"", 100, true, cap, ren));
    ASSERT_NE(rig.capPtr, nullptr);
    ASSERT_NE(rig.renderPtr, nullptr);
    EXPECT_EQ(rig.capPtr->lastOpenParams_.bufferMs, 30u);
    EXPECT_TRUE(rig.capPtr->lastOpenParams_.clientProperties.enabled);
    EXPECT_EQ(rig.capPtr->lastOpenParams_.clientProperties.category, AudioCategory::Speech);
    EXPECT_TRUE(rig.capPtr->lastOpenParams_.clientProperties.offload);
    EXPECT_EQ(rig.capPtr->lastOpenParams_.clientProperties.option, StreamOption::Ambisonics);
    EXPECT_TRUE(rig.renderPtr->lastOpenParams_.clientProperties.enabled);
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.clientProperties.category, AudioCategory::Media);
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.ducking,  DuckingMode::OptOut);
    eng.stop();
}

TEST(MonitorEngine, SetRenderParamsAppliesOnReengage) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, L"", L"", 100, true));   // params 全默认
    ASSERT_NE(rig.renderPtr, nullptr);
    EXPECT_FALSE(rig.renderPtr->lastOpenParams_.clientProperties.enabled);

    StreamParams np; np.clientProperties.enabled = true;
    np.clientProperties.option = StreamOption::Raw; np.bufferMs = 20;
    eng.setRenderParams(np);                    // 运行中改参数
    eng.setPlaybackEnabled(false);              // 关播放(disengage)
    // 等 pump 完成 disengage
    ASSERT_TRUE(waitFor([&]{ return eng.poll().renderState == StreamState::Idle; }))
        << "pump did not disengage render within timeout";
    eng.setPlaybackEnabled(true);               // 重新勾上 -> re-engage 用新参数
    // 等 pump 完成 re-engage：以 renderOpenDone >= 2 作为 acquire 屏障（release/acquire on the
    // same atomic synchronizes-with the open() write of lastOpenParams_ and the renderPtr store
    // in the factory — safer than the relaxed renderState_ store used by poll()).
    ASSERT_TRUE(waitFor([&]{ return rig.renderOpenDone.load(std::memory_order_acquire) >= 2; }))
        << "pump did not complete re-engage open within timeout";
    EXPECT_TRUE(rig.renderPtr->lastOpenParams_.clientProperties.enabled);
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.clientProperties.option, StreamOption::Raw);
    EXPECT_EQ(rig.renderPtr->lastOpenParams_.bufferMs, 20u);
    eng.stop();
}

TEST(MonitorEngine, CaptureFormatReachesBackend) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    AudioFormat want{96000, 2, 24, false};
    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, L"", L"", 50, false, {}, {}, &want));
    ASSERT_NE(rig.capPtr, nullptr);
    EXPECT_TRUE(rig.capPtr->sawRequested_);
    EXPECT_EQ(rig.capPtr->lastRequested_, want);
    eng.stop();
}

TEST(MonitorEngine, LegacyStartUsesEndpointCaptureSource) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, L"capture-id", L"render-id", 50,
                          false));

    EXPECT_EQ(rig.capOpenCount.load(), 1);
    EXPECT_TRUE(rig.sawCaptureSource);
    EXPECT_EQ(rig.lastCaptureSource.kind, CaptureSourceKind::Endpoint);
    EXPECT_EQ(rig.lastCaptureSource.deviceId, L"capture-id");
    EXPECT_EQ(rig.renderOpenCount.load(), 0);
    eng.stop();
}

TEST(MonitorEngine, LoopbackCaptureSourceReachesFactory) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"playback-render-id", 50,
                          false));

    EXPECT_EQ(rig.capOpenCount.load(), 1);
    EXPECT_TRUE(rig.sawCaptureSource);
    EXPECT_EQ(rig.lastCaptureSource.kind, CaptureSourceKind::SystemLoopback);
    EXPECT_EQ(rig.lastCaptureSource.deviceId, L"loopback-render-id");
    EXPECT_EQ(rig.renderOpenCount.load(), 0);
    eng.stop();
}

TEST(MonitorEngine, ApplicationLoopbackCaptureSourceReachesFactory) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::ApplicationLoopback, L"", 4242u};

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"", 50, false));

    EXPECT_EQ(rig.capOpenCount.load(), 1);
    EXPECT_TRUE(rig.sawCaptureSource);
    EXPECT_EQ(rig.lastCaptureSource.kind, CaptureSourceKind::ApplicationLoopback);
    EXPECT_TRUE(rig.lastCaptureSource.deviceId.empty());
    EXPECT_EQ(rig.lastCaptureSource.processId, 4242u);
    EXPECT_EQ(rig.renderOpenCount.load(), 0);
    eng.stop();
}

TEST(MonitorEngine, ApplicationLoopbackDoesNotStartSilentRender) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::ApplicationLoopback, L"", 4242u};

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"", 50, false));

    EXPECT_EQ(rig.silentOpenCount.load(), 0);
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Idle);
    eng.stop();
}

TEST(MonitorEngine, ApplicationLoopbackDefaultFactoryRejectsZeroPid) {
    MonitorEngine eng;
    CaptureSource source{CaptureSourceKind::ApplicationLoopback, L"", 0u};

    Result r = eng.start(BackendKind::WasapiShared, source, L"", 50, false);

    EXPECT_FALSE(r);
    MonitorStatus st = eng.poll();
    EXPECT_NE(st.overall, StreamState::Running);
    EXPECT_EQ(st.errorCode, static_cast<uint32_t>(MonitorError::CaptureOpen));
}

TEST(MonitorEngine, LoopbackStartsSilentRenderByDefault) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"", 50, false));

    EXPECT_EQ(rig.silentOpenCount.load(), 1);
    ASSERT_NE(rig.silentPtr, nullptr);
    EXPECT_TRUE(rig.silentPtr->sawRequested_);
    EXPECT_EQ(rig.silentPtr->lastRequested_, rig.capFmt);
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Running);
    eng.stop();
    EXPECT_TRUE(rig.silentStopped.load());
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Idle);
}

TEST(MonitorEngine, PollExposesCaptureProgressAndLoopbackSilenceFrames) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"", 50, false));
    ASSERT_NE(rig.capPtr, nullptr);

    rig.capPtr->idleSilenceFrames_ = 9600;
    rig.capPtr->silentPacketFrames_ = 4800;
    auto pcm = makeRampPcm({0, 0, 0, 0}, rig.capFmt.channels);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] { return eng.capWritten() >= 4; }));
    MonitorStatus st = eng.poll();
    EXPECT_GE(st.capWrittenFrames, 4u);
    EXPECT_EQ(st.loopbackIdleSilenceFrames, 9600u);
    EXPECT_EQ(st.loopbackSilentPacketFrames, 4800u);
    eng.stop();
}

TEST(MonitorEngine, LoopbackCanDisableSilentRender) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};
    LoopbackOptions opts{};
    opts.silentRender = false;

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"", 50, false,
                          {}, {}, nullptr, opts));

    EXPECT_EQ(rig.silentOpenCount.load(), 0);
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Idle);
    eng.stop();
}

TEST(MonitorEngine, SilentRenderFailureDoesNotFailLoopbackCapture) {
    FakeRig rig;
    rig.silentFailStart = true;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};

    Result r = eng.start(BackendKind::WasapiShared, source, L"", 50, false);

    EXPECT_TRUE(r);
    EXPECT_EQ(rig.capOpenCount.load(), 1);
    ASSERT_NE(rig.capPtr, nullptr);
    EXPECT_TRUE(rig.capPtr->started_.load());
    EXPECT_EQ(rig.silentOpenCount.load(), 1);
    EXPECT_EQ(eng.poll().overall, StreamState::Running);
    EXPECT_EQ(eng.poll().capState, StreamState::Running);
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Error);
    eng.stop();
}

TEST(MonitorEngine, SilentRenderOpenFailureDoesNotFailLoopbackCapture) {
    FakeRig rig;
    rig.silentFailOpen = true;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"loopback-render-id"};

    Result r = eng.start(BackendKind::WasapiShared, source, L"", 50, false);

    EXPECT_TRUE(r);
    ASSERT_NE(rig.capPtr, nullptr);
    EXPECT_TRUE(rig.capPtr->started_.load());
    EXPECT_EQ(eng.poll().capState, StreamState::Running);
    EXPECT_EQ(rig.silentOpenCount.load(), 1);
    EXPECT_EQ(eng.poll().overall, StreamState::Running);
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Error);
    eng.stop();
    EXPECT_EQ(eng.poll().silentRenderState, StreamState::Idle);
}

TEST(MonitorEngine, LoopbackPlaybackRejectsSameRenderDevice) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"same-render-id"};

    Result r = eng.start(BackendKind::WasapiShared, source, L"same-render-id", 50, true);

    EXPECT_FALSE(r);
    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.overall, StreamState::Error);
    EXPECT_EQ(st.errorCode, static_cast<uint32_t>(MonitorError::LoopbackFeedback));
    EXPECT_EQ(rig.capOpenCount.load(), 0);
    EXPECT_EQ(rig.renderOpenCount.load(), 0);
}

TEST(MonitorEngine, LoopbackPlaybackRejectsBothDefaultRenderDevices) {
    FakeRig rig;
    MonitorEngine eng(rig.factory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L""};

    Result r = eng.start(BackendKind::WasapiShared, source, L"", 50, true);

    EXPECT_FALSE(r);
    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.overall, StreamState::Error);
    EXPECT_EQ(st.errorCode, static_cast<uint32_t>(MonitorError::LoopbackFeedback));
    EXPECT_EQ(rig.capOpenCount.load(), 0);
    EXPECT_EQ(rig.renderOpenCount.load(), 0);
}

TEST(MonitorEngine, LoopbackRuntimePlaybackRejectsSameRenderDevice) {
    FakeRig rig;
    MonitorEngine eng(rig.factory(), rig.silentFactory());
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"same-render-id"};

    ASSERT_TRUE(eng.start(BackendKind::WasapiShared, source, L"same-render-id", 50,
                          false));
    ASSERT_EQ(rig.capOpenCount.load(), 1);
    ASSERT_EQ(rig.renderOpenCount.load(), 0);
    ASSERT_NE(rig.capPtr, nullptr);

    eng.setPlaybackEnabled(true);
    std::vector<int16_t> ramp(1024, 3);
    auto pcm = makeRampPcm(ramp, rig.capFmt.channels);
    rig.capPtr->pushPcm(pcm.data(), pcm.size());

    ASSERT_TRUE(waitFor([&] {
        return eng.poll().renderState == StreamState::Error;
    })) << "runtime playback feedback guard did not reject render engage";

    MonitorStatus st = eng.poll();
    EXPECT_EQ(st.overall, StreamState::Running);
    EXPECT_EQ(st.capState, StreamState::Running);
    EXPECT_EQ(st.renderState, StreamState::Error);
    EXPECT_EQ(st.errorCode, static_cast<uint32_t>(MonitorError::LoopbackFeedback));
    EXPECT_EQ(rig.renderOpenCount.load(), 0);
    eng.stop();
}
