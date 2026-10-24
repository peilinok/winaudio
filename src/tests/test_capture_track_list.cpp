#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "CaptureTrackList.h"
#include "RingBuffer.h"
#include "TrackScopeReader.h"
#include "WavFile.h"
#include "WavSinkOutput.h"

using namespace wa;

namespace {

class FakeBackend : public IAudioBackend {
public:
    FakeBackend(AudioFormat fmt, bool failStart, std::atomic<bool>* stoppedOut, bool failOpen)
        : fmt_(fmt), failStart_(failStart), stoppedOut_(stoppedOut), failOpen_(failOpen) {}

    Result open(const DeviceId&, const AudioFormat&, RingBuffer* ring,
                const StreamParams& params) override {
        if (failOpen_) return Result::Fail(122, "fake: open failed");
        ring_ = ring;
        lastOpenParams_ = params;
        return Result::Ok();
    }
    Result start() override {
        if (failStart_) return Result::Fail(123, "fake: start failed");
        return Result::Ok();
    }
    void stop() override {
        if (stoppedOut_) stoppedOut_->store(true, std::memory_order_relaxed);
    }
    void close() override {}
    BackendStats stats() const override {
        BackendStats s{};
        s.actualFormat = fmt_;
        s.runtimeError = runtimeHr_.load(std::memory_order_relaxed);
        return s;
    }
    bool inError() const override { return runtimeHr_.load(std::memory_order_relaxed) < 0; }
    void failRuntime(long hr) { runtimeHr_.store(hr, std::memory_order_relaxed); }
    std::atomic<long> runtimeHr_{0};

    AudioFormat        fmt_;
    bool               failStart_ = false;
    std::atomic<bool>* stoppedOut_ = nullptr;
    bool               failOpen_ = false;
    RingBuffer*        ring_ = nullptr;
    StreamParams       lastOpenParams_{};
    AudioFormat        lastRequested_{};
    bool               sawRequested_ = false;

    void pushPcm(const void* data, size_t bytes) {
        if (ring_) ring_->write(data, bytes);
    }
};

struct ListRig {
    AudioFormat fmt{48000, 2, 16, false};
    bool failOpen = false;
    bool failStart = false;
    std::vector<FakeBackend*> caps;
    std::vector<FakeBackend*> silents;
    std::vector<std::unique_ptr<std::atomic<bool>>> stopped;

    CaptureTrackList::BackendFactory factory() {
        return [this](const CaptureSource&, const AudioFormat* req) {
            stopped.push_back(std::make_unique<std::atomic<bool>>(false));
            auto b = std::make_unique<FakeBackend>(fmt, failStart, stopped.back().get(), failOpen);
            b->sawRequested_ = req != nullptr;
            if (req) b->lastRequested_ = *req;
            caps.push_back(b.get());
            return b;
        };
    }

    CaptureTrackList::SilentRenderFactory silentFactory() {
        return [this](const AudioFormat* req) {
            stopped.push_back(std::make_unique<std::atomic<bool>>(false));
            auto b = std::make_unique<FakeBackend>(fmt, false, stopped.back().get(), false);
            b->sawRequested_ = req != nullptr;
            if (req) b->lastRequested_ = *req;
            silents.push_back(b.get());
            return b;
        };
    }
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

std::wstring tempWav(const wchar_t* tag) {
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"wa_ctl_" + tag + L"_" + std::to_wstring(GetTickCount64()) + L".wav";
}

} // namespace

TEST(CaptureTrackList, CreateStartsLiveTrack) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId id = 0;
    CaptureTrackCreate spec{};
    spec.source.kind = CaptureSourceKind::Endpoint;
    Result r = list.create(spec, &id);
    ASSERT_TRUE(r) << r.message;
    EXPECT_NE(id, 0u);
    auto st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0].id, id);
    EXPECT_EQ(st[0].state, StreamState::Running);
    EXPECT_EQ(st[0].source.kind, CaptureSourceKind::Endpoint);
    list.destroyAll();
}

TEST(CaptureTrackList, DestroyRemovesTrackAndStopsBackend) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId id = 0;
    ASSERT_TRUE(list.create({}, &id));
    ASSERT_EQ(list.poll().size(), 1u);
    list.destroy(id);
    EXPECT_TRUE(list.poll().empty());
    ASSERT_FALSE(rig.stopped.empty());
    EXPECT_TRUE(rig.stopped.back()->load(std::memory_order_relaxed));
}

TEST(CaptureTrackList, RuntimeBackendFaultMarksTrackError) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId id = 0;
    CaptureTrackCreate spec{};
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(rig.caps.size(), 1u);
    EXPECT_EQ(list.poll()[0].state, StreamState::Running);

    rig.caps[0]->failRuntime(static_cast<long>(static_cast<int32_t>(0x88890004u))); // AUDCLNT_E_DEVICE_INVALIDATED
    ASSERT_TRUE(waitFor([&] { return list.poll()[0].state == StreamState::Error; }));
    const auto st = list.poll();
    ASSERT_EQ(st.size(), 1u) << "a faulted track stays listed until Destroy";
    EXPECT_NE(st[0].message.find("AUDCLNT_E_DEVICE_INVALIDATED"), std::string::npos)
        << st[0].message;
    EXPECT_FALSE(rig.stopped[0]->load()) << "the list does not stop the backend by itself";

    list.destroy(id);
    EXPECT_TRUE(list.poll().empty());
    EXPECT_TRUE(rig.stopped[0]->load());
}

TEST(CaptureTrackList, RuntimeFaultStillFinalizesCreateTimeWav) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.wavPath = tempWav(L"fault");
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(rig.caps.size(), 1u);
    const int16_t pcm[8] = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};
    rig.caps[0]->pushPcm(pcm, sizeof(pcm));
    ASSERT_TRUE(waitFor([&] { return list.written(id) >= 4; }));

    rig.caps[0]->failRuntime(static_cast<long>(static_cast<int32_t>(0x88890004u)));
    ASSERT_TRUE(waitFor([&] { return list.poll()[0].state == StreamState::Error; }));
    list.destroy(id);

    wa::WavReader r;
    ASSERT_TRUE(r.open(spec.wavPath)) << "WAV written before the fault must stay readable";
    std::vector<int16_t> back(8, 0);
    EXPECT_EQ(r.read(back.data(), sizeof(pcm)), sizeof(pcm));
    EXPECT_EQ(std::vector<int16_t>(pcm, pcm + 8), back);
    r.close();
    DeleteFileW(spec.wavPath.c_str());
}

TEST(CaptureTrackList, SilentRenderRuntimeFaultKeepsCaptureRunning) {
    ListRig rig;
    CaptureTrackList list(rig.factory(), rig.silentFactory());
    TrackId id = 0;
    CaptureTrackCreate spec{};
    spec.source.kind = CaptureSourceKind::SystemLoopback;
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(rig.silents.size(), 1u);
    EXPECT_EQ(list.poll()[0].silentRenderState, StreamState::Running);

    rig.silents[0]->failRuntime(static_cast<long>(static_cast<int32_t>(0x88890004u)));
    ASSERT_TRUE(waitFor([&] { return list.poll()[0].silentRenderState == StreamState::Error; }));
    EXPECT_EQ(list.poll()[0].state, StreamState::Running);
}

TEST(CaptureTrackList, DestroyAllClearsEveryMember) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    ASSERT_TRUE(list.create({}, nullptr));
    ASSERT_TRUE(list.create({}, nullptr));
    EXPECT_EQ(list.poll().size(), 2u);
    list.destroyAll();
    EXPECT_TRUE(list.poll().empty());
}

TEST(CaptureTrackList, FailedOpenDoesNotLeaveMember) {
    ListRig rig;
    rig.failOpen = true;
    CaptureTrackList list(rig.factory());
    TrackId id = 99;
    Result r = list.create({}, &id);
    EXPECT_FALSE(r);
    EXPECT_TRUE(list.poll().empty());
}

TEST(CaptureTrackList, FailedStartDoesNotLeaveMember) {
    ListRig rig;
    rig.failStart = true;
    CaptureTrackList list(rig.factory());
    Result r = list.create({}, nullptr);
    EXPECT_FALSE(r);
    EXPECT_TRUE(list.poll().empty());
}

TEST(CaptureTrackList, EqualSourcesMayBothBeLive) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.source.kind = CaptureSourceKind::SystemLoopback;
    spec.source.deviceId = L"same-endpoint";
    TrackId a = 0, b = 0;
    ASSERT_TRUE(list.create(spec, &a));
    ASSERT_TRUE(list.create(spec, &b));
    EXPECT_NE(a, b);
    EXPECT_EQ(list.poll().size(), 2u);
    list.destroyAll();
}

TEST(CaptureTrackList, CreateAllowedWhileSiblingStillListed) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId a = 0, b = 0;
    ASSERT_TRUE(list.create({}, &a));
    ASSERT_TRUE(list.create({}, &b));
    list.destroy(a);
    TrackId c = 0;
    ASSERT_TRUE(list.create({}, &c));
    auto st = list.poll();
    ASSERT_EQ(st.size(), 2u);
    list.destroyAll();
}

TEST(CaptureTrackList, WavOpenFailureDoesNotDestroySibling) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate ok{};
    ok.wavPath = tempWav(L"ok");
    TrackId good = 0;
    ASSERT_TRUE(list.create(ok, &good));

    CaptureTrackCreate bad{};
    bad.wavPath = L"?:\\wa_ctl_not_a_path.wav";
    TrackId badId = 0;
    ASSERT_TRUE(list.create(bad, &badId));

    ASSERT_TRUE(waitFor([&] {
        auto st = list.poll();
        if (st.size() != 2u) return false;
        bool sawErr = false, sawRun = false;
        for (const auto& s : st) {
            if (s.id == badId && s.state == StreamState::Error) sawErr = true;
            if (s.id == good && s.state == StreamState::Running) sawRun = true;
        }
        return sawErr && sawRun;
    })) << "expected WAV error isolated to one Track";

    list.destroyAll();
    DeleteFileW(ok.wavPath.c_str());
}

TEST(CaptureTrackList, ExclusiveLoopbackRejected) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.kind = BackendKind::WasapiExclusive;
    spec.source.kind = CaptureSourceKind::SystemLoopback;
    EXPECT_FALSE(list.create(spec, nullptr));
    EXPECT_TRUE(list.poll().empty());
}

TEST(CaptureTrackList, ExclusiveApplicationLoopbackRejected) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.kind = BackendKind::WasapiExclusive;
    spec.source.kind = CaptureSourceKind::ApplicationLoopback;
    spec.source.processId = 4242;
    EXPECT_FALSE(list.create(spec, nullptr));
    EXPECT_TRUE(list.poll().empty());
}

TEST(CaptureTrackList, ApplicationLoopbackPreservesPidAndMode) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.source.kind = CaptureSourceKind::ApplicationLoopback;
    spec.source.processId = 4242;
    spec.source.processLoopbackMode = ProcessLoopbackMode::ExcludeTree;
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    auto st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0].source.kind, CaptureSourceKind::ApplicationLoopback);
    EXPECT_EQ(st[0].source.processId, 4242u);
    EXPECT_EQ(st[0].source.processLoopbackMode, ProcessLoopbackMode::ExcludeTree);

    CaptureTrackCreate other = spec;
    other.source.processLoopbackMode = ProcessLoopbackMode::IncludeTree;
    TrackId b = 0;
    ASSERT_TRUE(list.create(other, &b));
    TrackId c = 0;
    ASSERT_TRUE(list.create(spec, &c));
    EXPECT_EQ(list.poll().size(), 3u);
    list.destroyAll();
}

TEST(CaptureTrackList, CreateFailureDoesNotDestroySibling) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId good = 0;
    ASSERT_TRUE(list.create({}, &good));
    rig.failOpen = true;
    EXPECT_FALSE(list.create({}, nullptr));
    auto st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0].id, good);
    EXPECT_EQ(st[0].state, StreamState::Running);
    list.destroyAll();
}

TEST(CaptureTrackList, SecondListUnaffectedByDestroyAll) {
    ListRig a, b;
    CaptureTrackList listA(a.factory());
    CaptureTrackList listB(b.factory());
    ASSERT_TRUE(listA.create({}, nullptr));
    ASSERT_TRUE(listB.create({}, nullptr));
    listA.destroyAll();
    EXPECT_TRUE(listA.poll().empty());
    ASSERT_EQ(listB.poll().size(), 1u);
    EXPECT_EQ(listB.poll()[0].state, StreamState::Running);
    listB.destroyAll();
}

TEST(CaptureTrackList, TapsAreIndependent) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId a = 0, b = 0;
    ASSERT_TRUE(list.create({}, &a));
    ASSERT_TRUE(list.create({}, &b));
    ASSERT_EQ(rig.caps.size(), 2u);

    const int16_t one[4] = {1000, 1000, 1000, 1000};
    const int16_t two[8] = {2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000};
    rig.caps[0]->pushPcm(one, sizeof(one));
    rig.caps[1]->pushPcm(two, sizeof(two));

    ASSERT_TRUE(waitFor([&] { return list.written(a) >= 2 && list.written(b) >= 4; }));
    EXPECT_GE(list.written(a), 2u);
    EXPECT_GE(list.written(b), 4u);
    EXPECT_EQ(list.tapChannels(a), 2);
    float sa[2] = {};
    float sb[4] = {};
    uint64_t ea = 0, eb = 0;
    ASSERT_TRUE(list.snapshotLatest(a, 2, sa, ea));
    ASSERT_TRUE(list.snapshotLatest(b, 4, sb, eb));
    EXPECT_NEAR(sa[0], 1000.f / 32768.f, 1e-4f);
    EXPECT_NEAR(sb[0], 2000.f / 32768.f, 1e-4f);
    {
        TrackScopeReader ra(list, a);
        EXPECT_EQ(ra.channels(), 2);
        EXPECT_GE(ra.written(), 2u);
    }
    list.destroy(a);
    EXPECT_EQ(list.written(a), 0u);
    EXPECT_GE(list.written(b), 4u);
    list.destroyAll();
}

TEST(CaptureTrackList, CreateTimeWavWritesPushedPcm) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.wavPath = tempWav(L"pcm");
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(rig.caps.size(), 1u);
    const int16_t pcm[8] = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};
    rig.caps[0]->pushPcm(pcm, sizeof(pcm));
    ASSERT_TRUE(waitFor([&] { return list.written(id) >= 4; }));
    list.destroy(id);

    wa::WavReader r;
    ASSERT_TRUE(r.open(spec.wavPath));
    std::vector<int16_t> back(8, 0);
    EXPECT_EQ(r.read(back.data(), sizeof(pcm)), sizeof(pcm));
    EXPECT_EQ(std::vector<int16_t>(pcm, pcm + 8), back);
    r.close();
    DeleteFileW(spec.wavPath.c_str());
}

TEST(CaptureTrackList, LiveDumpWritesPushedPcm) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId id = 0;
    ASSERT_TRUE(list.create({}, &id));
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    ASSERT_TRUE(list.startDump(id, dir));
    auto st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_TRUE(st[0].dumping);
    EXPECT_FALSE(st[0].dumpFileName.empty());

    const int16_t pcm[8] = {11, 22, 33, 44, 55, 66, 77, 88};
    rig.caps[0]->pushPcm(pcm, sizeof(pcm));
    ASSERT_TRUE(waitFor([&] { return list.written(id) >= 4; }));
    ASSERT_TRUE(list.stopDump(id));
    st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_FALSE(st[0].dumping);
    EXPECT_EQ(st[0].state, StreamState::Running);

    wa::WavReader r;
    ASSERT_TRUE(r.open(st[0].dumpPath));
    std::vector<int16_t> back(8, 0);
    EXPECT_EQ(r.read(back.data(), sizeof(pcm)), sizeof(pcm));
    EXPECT_EQ(std::vector<int16_t>(pcm, pcm + 8), back);
    r.close();
    DeleteFileW(st[0].dumpPath.c_str());
    list.destroy(id);
}

TEST(CaptureTrackList, AppLoopbackDumpNameIncludesPidAndProcessName) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.source.kind = CaptureSourceKind::ApplicationLoopback;
    spec.source.processId = 4242;
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    ASSERT_TRUE(list.startDump(id, dir));
    const std::wstring name = list.poll()[0].dumpFileName;
    list.stopDump(id);
    DeleteFileW(list.poll()[0].dumpPath.c_str());
    list.destroy(id);

    // Name lookup may fail in CI (no such PID) and fall back to "unknown".
    EXPECT_EQ(name.find(L"app-loopback_"), 0u);
    EXPECT_NE(name.find(L"4242"), std::wstring::npos);
}

TEST(CaptureTrackList, LiveDumpRejectedWhenAlreadyDumping) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId id = 0;
    ASSERT_TRUE(list.create({}, &id));
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    ASSERT_TRUE(list.startDump(id, dir));
    EXPECT_FALSE(list.startDump(id, dir));
    list.stopDump(id);
    DeleteFileW(list.poll()[0].dumpPath.c_str());
    list.destroy(id);
}

TEST(CaptureTrackList, LiveDumpRejectedWhenTrackNotRunning) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.wavPath = L"?:\\wa_ctl_not_a_path.wav";
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(list.poll()[0].state, StreamState::Error);
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    EXPECT_FALSE(list.startDump(id, dir));
    list.destroy(id);
}

TEST(CaptureTrackList, LiveDumpOpenFailureLeavesTrackRunning) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId id = 0;
    ASSERT_TRUE(list.create({}, &id));
    EXPECT_FALSE(list.startDump(id, L"?:\\not-a-folder"));
    auto st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0].state, StreamState::Running);
    EXPECT_FALSE(st[0].dumping);
    list.destroy(id);
}

TEST(CaptureTrackList, DestroyWhileDumpingFinalizesWav) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId id = 0;
    ASSERT_TRUE(list.create({}, &id));
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    ASSERT_TRUE(list.startDump(id, dir));
    const int16_t pcm[4] = {1, 2, 3, 4};
    rig.caps[0]->pushPcm(pcm, sizeof(pcm));
    ASSERT_TRUE(waitFor([&] { return list.written(id) >= 2; }));
    const std::wstring path = list.poll()[0].dumpPath;
    list.destroy(id);
    EXPECT_TRUE(list.poll().empty());
    wa::WavReader r;
    ASSERT_TRUE(r.open(path));
    r.close();
    DeleteFileW(path.c_str());
}

TEST(CaptureTrackList, OptionalWavStillRuns) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.wavPath.clear();
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    EXPECT_EQ(list.poll()[0].state, StreamState::Running);
    list.destroyAll();
}

TEST(CaptureTrackList, DefaultCreateOpensWithDefaultStreamParamsAndNoRequestedFormat) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    TrackId id = 0;
    ASSERT_TRUE(list.create({}, &id));
    ASSERT_EQ(rig.caps.size(), 1u);
    EXPECT_TRUE(rig.caps[0]->lastOpenParams_.isDefault());
    EXPECT_FALSE(rig.caps[0]->sawRequested_);
    list.destroyAll();
}

TEST(CaptureTrackList, CreatePassesRequestedFormatToBackend) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    AudioFormat want{44100, 1, 24, false};
    CaptureTrackCreate spec{};
    spec.requested = &want;
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(rig.caps.size(), 1u);
    EXPECT_TRUE(rig.caps[0]->sawRequested_);
    EXPECT_EQ(rig.caps[0]->lastRequested_, want);
    list.destroyAll();
}

TEST(CaptureTrackList, CreatePassesStreamParamsToOpen) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate spec{};
    spec.streamParams.clientProperties.enabled = true;
    spec.streamParams.clientProperties.category = AudioCategory::Speech;
    spec.streamParams.clientProperties.offload = true;
    spec.streamParams.clientProperties.option = StreamOption::Raw;
    spec.streamParams.bufferMs = 50;
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(rig.caps.size(), 1u);
    const StreamParams& got = rig.caps[0]->lastOpenParams_;
    EXPECT_TRUE(got.clientProperties.enabled);
    EXPECT_EQ(got.clientProperties.category, AudioCategory::Speech);
    EXPECT_TRUE(got.clientProperties.offload);
    EXPECT_EQ(got.clientProperties.option, StreamOption::Raw);
    EXPECT_EQ(got.bufferMs, 50u);
    EXPECT_FALSE(got.isDefault());
    list.destroyAll();
}

TEST(CaptureTrackList, SystemAndApplicationLoopbackBothPassStreamParams) {
    ListRig rig;
    CaptureTrackList list(rig.factory());
    CaptureTrackCreate sys{};
    sys.source.kind = CaptureSourceKind::SystemLoopback;
    sys.loopbackOptions.silentRender = false;
    sys.streamParams.bufferMs = 20;
    TrackId a = 0;
    ASSERT_TRUE(list.create(sys, &a));

    CaptureTrackCreate app{};
    app.source.kind = CaptureSourceKind::ApplicationLoopback;
    app.source.processId = 4242;
    app.streamParams.clientProperties.enabled = true;
    app.streamParams.clientProperties.category = AudioCategory::Media;
    TrackId b = 0;
    ASSERT_TRUE(list.create(app, &b));

    ASSERT_EQ(rig.caps.size(), 2u);
    EXPECT_EQ(rig.caps[0]->lastOpenParams_.bufferMs, 20u);
    EXPECT_TRUE(rig.caps[1]->lastOpenParams_.clientProperties.enabled);
    EXPECT_EQ(rig.caps[1]->lastOpenParams_.clientProperties.category, AudioCategory::Media);
    list.destroyAll();
}

TEST(CaptureTrackList, SilentRenderOpenStaysDefaultWhenCaptureHasStreamParams) {
    ListRig rig;
    CaptureTrackList list(rig.factory(), rig.silentFactory());
    CaptureTrackCreate spec{};
    spec.source.kind = CaptureSourceKind::SystemLoopback;
    spec.loopbackOptions.silentRender = true;
    spec.streamParams.bufferMs = 80;
    spec.streamParams.clientProperties.enabled = true;
    AudioFormat want{48000, 2, 16, false};
    spec.requested = &want;
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(rig.caps.size(), 1u);
    ASSERT_EQ(rig.silents.size(), 1u);
    EXPECT_EQ(rig.caps[0]->lastOpenParams_.bufferMs, 80u);
    EXPECT_TRUE(rig.caps[0]->lastOpenParams_.clientProperties.enabled);
    EXPECT_TRUE(rig.caps[0]->sawRequested_);
    EXPECT_TRUE(rig.silents[0]->lastOpenParams_.isDefault());
    EXPECT_FALSE(rig.silents[0]->sawRequested_);
    list.destroyAll();
}

namespace {
class FailWriteOutput : public WavSinkOutput {
public:
    Result open(const std::wstring& path, const AudioFormat& fmt) override {
        return writer_.open(path, fmt);
    }
    Result setBuffer(size_t bytes) override { writer_.setStdioBuffer(bytes); return Result::Ok(); }
    Result write(const void*, size_t) override { return Result::Fail(101, "injected write failure"); }
    Result close() override { return writer_.close(); }
private:
    WavWriter writer_;
};

enum class DumpFailure { Open, Write, Close };

class OptionalFailureOutput : public WavSinkOutput {
public:
    explicit OptionalFailureOutput(DumpFailure failure) : failure_(failure) {}
    Result open(const std::wstring& path, const AudioFormat& fmt) override {
        return failure_ == DumpFailure::Open ? Result::Fail(101, "injected open failure")
                                             : writer_.open(path, fmt);
    }
    Result setBuffer(size_t bytes) override { return writer_.setStdioBuffer(bytes); }
    Result write(const void* data, size_t bytes) override {
        return failure_ == DumpFailure::Write ? Result::Fail(102, "injected write failure")
                                              : writer_.write(data, bytes);
    }
    Result close() override {
        const Result result = writer_.close();
        return failure_ == DumpFailure::Close ? Result::Fail(103, "injected close failure") : result;
    }
private:
    DumpFailure failure_;
    WavWriter writer_;
};

struct SlowOutputGate {
    std::mutex mtx;
    std::condition_variable cv;
    bool blockWrite = false;
    bool entered = false;
    bool released = false;
    bool waitEntered() {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, std::chrono::seconds(10), [&] { return entered; });
    }
    void pause() {
        std::unique_lock<std::mutex> lk(mtx);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return released; });
    }
    void release() {
        std::lock_guard<std::mutex> lk(mtx);
        released = true;
        cv.notify_all();
    }
};

struct SlowOutputRelease {
    SlowOutputGate& gate;
    ~SlowOutputRelease() { gate.release(); }
};

class SlowOutput : public WavSinkOutput {
public:
    explicit SlowOutput(SlowOutputGate& gate) : gate_(gate) {}
    Result open(const std::wstring& path, const AudioFormat& fmt) override {
        return writer_.open(path, fmt);
    }
    Result setBuffer(size_t bytes) override { return writer_.setStdioBuffer(bytes); }
    Result write(const void* data, size_t bytes) override {
        if (gate_.blockWrite) gate_.pause();
        return writer_.write(data, bytes);
    }
    Result close() override {
        if (!gate_.blockWrite) gate_.pause();
        return writer_.close();
    }
private:
    SlowOutputGate& gate_;
    WavWriter writer_;
};
}

TEST(CaptureTrackList, RequiredWavAsyncFailureMarksOnlyItsTrackWithoutMorePcm) {
    ListRig rig;
    CaptureTrackList list(rig.factory(), {}, [] { return std::make_unique<FailWriteOutput>(); });
    TrackId required = 0, sibling = 0;
    CaptureTrackCreate spec;
    spec.wavPath = tempWav(L"required_async");
    ASSERT_TRUE(list.create(spec, &required));
    ASSERT_TRUE(list.create({}, &sibling));
    const std::vector<int16_t> pcm(256u * 1024u / 2u, 123);
    rig.caps[0]->pushPcm(pcm.data(), pcm.size() * sizeof(int16_t));
    ASSERT_TRUE(waitFor([&] { return list.poll()[0].dumpError; }));
    EXPECT_TRUE(waitFor([&] { return list.poll()[0].state == StreamState::Error; }));
    EXPECT_EQ(list.poll()[1].state, StreamState::Running);
    EXPECT_FALSE(rig.stopped[0]->load());
    EXPECT_FALSE(rig.stopped[1]->load());
    list.destroyAll();
    DeleteFileW(spec.wavPath.c_str());
}

TEST(CaptureTrackList, BothLoopbackKindsKeepCapturingAcrossThreeIndependentDumps) {
    for (const auto kind : {CaptureSourceKind::SystemLoopback, CaptureSourceKind::ApplicationLoopback}) {
        ListRig rig;
        CaptureTrackList list(rig.factory());
        CaptureTrackCreate spec;
        spec.source.kind = kind;
        spec.source.processId = 4242;
        spec.loopbackOptions.silentRender = false;
        TrackId id = 0, sibling = 0;
        ASSERT_TRUE(list.create(spec, &id));
        ASSERT_TRUE(list.create(spec, &sibling));
        std::vector<std::wstring> paths;
        wchar_t dir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, dir);
        uint64_t frames = 0;
        for (int round = 0; round < 3; ++round) {
            ASSERT_TRUE(list.startDump(id, dir));
            paths.push_back(list.poll()[0].dumpPath);
            const int16_t pcm[4]{static_cast<int16_t>(100 + round), -100, 200, -200};
            rig.caps[0]->pushPcm(pcm, sizeof(pcm));
            frames += 2;
            ASSERT_TRUE(waitFor([&] { return list.written(id) == frames; }));
            ASSERT_TRUE(list.stopDump(id));
            WavReader reader;
            ASSERT_TRUE(reader.open(paths.back()));
            EXPECT_TRUE(reader.format() == rig.fmt);
            int16_t back[4]{};
            EXPECT_EQ(reader.read(back, sizeof(back)), sizeof(back));
            EXPECT_EQ(std::vector<int16_t>(back, back + 4), std::vector<int16_t>(pcm, pcm + 4));
            EXPECT_EQ(reader.read(back, sizeof(back)), 0u);
            reader.close();
            // PCM after every stop still advances both taps and both devices remain up.
            rig.caps[0]->pushPcm(pcm, sizeof(pcm));
            rig.caps[1]->pushPcm(pcm, sizeof(pcm));
            frames += 2;
            ASSERT_TRUE(waitFor([&] { return list.written(id) == frames
                                          && list.written(sibling) == static_cast<uint64_t>((round + 1) * 2); }));
            EXPECT_FALSE(rig.stopped[0]->load());
            EXPECT_FALSE(rig.stopped[1]->load());
            EXPECT_EQ(list.poll()[0].state, StreamState::Running);
        }
        EXPECT_NE(paths[0], paths[1]);
        EXPECT_NE(paths[1], paths[2]);
        list.destroyAll();
        for (const auto& path : paths) DeleteFileW(path.c_str());
    }
}

TEST(CaptureTrackList, OptionalOpenWriteAndCloseFailuresKeepBothTracksAndTapsRunning) {
    for (const auto failure : {DumpFailure::Open, DumpFailure::Write, DumpFailure::Close}) {
        ListRig rig;
        CaptureTrackList list(rig.factory(), {}, [=] {
            return std::make_unique<OptionalFailureOutput>(failure);
        });
        TrackId id = 0, sibling = 0;
        ASSERT_TRUE(list.create({}, &id));
        ASSERT_TRUE(list.create({}, &sibling));
        wchar_t dir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, dir);
        const Result started = list.startDump(id, dir);
        EXPECT_EQ(started.ok, failure != DumpFailure::Open);
        const auto path = list.poll()[0].dumpPath;
        if (started) {
            const std::vector<int16_t> pcm(256u * 1024u / 2u, 100);
            rig.caps[0]->pushPcm(pcm.data(), pcm.size() * 2);
            ASSERT_TRUE(waitFor([&] { return list.written(id) == pcm.size() / 2; }));
            EXPECT_FALSE(list.stopDump(id));
        }
        const uint64_t before = list.written(id);
        const int16_t pcm[4]{11, -11, 22, -22};
        rig.caps[0]->pushPcm(pcm, sizeof(pcm));
        rig.caps[1]->pushPcm(pcm, sizeof(pcm));
        ASSERT_TRUE(waitFor([&] { return list.written(id) == before + 2 && list.written(sibling) == 2; }));
        EXPECT_EQ(list.poll()[0].state, StreamState::Running);
        EXPECT_EQ(list.poll()[1].state, StreamState::Running);
        EXPECT_TRUE(list.poll()[0].dumpError);
        EXPECT_FALSE(rig.stopped[0]->load());
        EXPECT_FALSE(rig.stopped[1]->load());
        list.destroyAll();
        DeleteFileW(path.c_str());
    }
}

TEST(CaptureTrackList, DestroyAllAndDestructorFinalizeEveryAcceptedByte) {
    for (const bool explicitDestroy : {true, false}) {
        ListRig rig;
        const auto a = tempWav(explicitDestroy ? L"destroy_all_a" : L"destructor_a");
        const auto b = tempWav(explicitDestroy ? L"destroy_all_b" : L"destructor_b");
        const int16_t pcm[4]{1, -2, 3, -4};
        {
            CaptureTrackList list(rig.factory());
            CaptureTrackCreate spec;
            TrackId first = 0, second = 0;
            spec.wavPath = a;
            ASSERT_TRUE(list.create(spec, &first));
            spec.wavPath = b;
            ASSERT_TRUE(list.create(spec, &second));
            for (auto cap : rig.caps) cap->pushPcm(pcm, sizeof(pcm));
            ASSERT_TRUE(waitFor([&] { return list.written(first) == 2 && list.written(second) == 2; }));
            if (explicitDestroy) list.destroyAll();
        }
        EXPECT_TRUE(rig.stopped[0]->load());
        EXPECT_TRUE(rig.stopped[1]->load());
        for (const auto& path : {a, b}) {
            WavReader reader;
            ASSERT_TRUE(reader.open(path));
            int16_t back[4]{};
            EXPECT_EQ(reader.read(back, sizeof(back)), sizeof(back));
            EXPECT_EQ(std::vector<int16_t>(back, back + 4), std::vector<int16_t>(pcm, pcm + 4));
            reader.close();
            DeleteFileW(path.c_str());
        }
    }
}

TEST(CaptureTrackList, SlowDumpCloseKeepsBothCaptureTapsAndDevicesAdvancing) {
    ListRig rig;
    SlowOutputGate gate;
    CaptureTrackList list(rig.factory(), {}, [&] { return std::make_unique<SlowOutput>(gate); });
    SlowOutputRelease release{gate}; // Release before list teardown on an early assertion.
    TrackId id = 0, sibling = 0;
    ASSERT_TRUE(list.create({}, &id));
    ASSERT_TRUE(list.create({}, &sibling));
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    ASSERT_TRUE(list.startDump(id, dir));
    const auto path = list.poll()[0].dumpPath;
    const int16_t pcm[4]{10, -10, 20, -20};
    rig.caps[0]->pushPcm(pcm, sizeof(pcm));
    ASSERT_TRUE(waitFor([&] { return list.written(id) == 2; }));
    Result stopped;
    std::thread control([&] { stopped = list.stopDump(id); });
    EXPECT_TRUE(gate.waitEntered()) << "writer close deadlock";
    rig.caps[0]->pushPcm(pcm, sizeof(pcm));
    rig.caps[1]->pushPcm(pcm, sizeof(pcm));
    EXPECT_TRUE(waitFor([&] { return list.written(id) == 4 && list.written(sibling) == 2; }));
    EXPECT_FALSE(rig.stopped[0]->load());
    EXPECT_FALSE(rig.stopped[1]->load());
    gate.release();
    control.join();
    EXPECT_TRUE(stopped);
    WavReader reader;
    ASSERT_TRUE(reader.open(path));
    int16_t back[4]{};
    EXPECT_EQ(reader.read(back, sizeof(back)), sizeof(back));
    EXPECT_EQ(std::vector<int16_t>(back, back + 4), std::vector<int16_t>(pcm, pcm + 4));
    EXPECT_EQ(reader.read(back, sizeof(back)), 0u);
    reader.close();
    list.destroyAll();
    DeleteFileW(path.c_str());
}

TEST(CaptureTrackList, OptionalOverflowKeepsCapturingAndDrainsTheAcceptedPrefix) {
    ListRig rig;
    SlowOutputGate gate;
    gate.blockWrite = true;
    CaptureTrackList list(rig.factory(), {}, [&] { return std::make_unique<SlowOutput>(gate); });
    SlowOutputRelease release{gate};
    TrackId id = 0, sibling = 0;
    ASSERT_TRUE(list.create({}, &id));
    ASSERT_TRUE(list.create({}, &sibling));
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    ASSERT_TRUE(list.startDump(id, dir));
    const auto path = list.poll()[0].dumpPath;
    const std::vector<int16_t> block(8192, 123);
    for (uint64_t n = 1; n <= 96; ++n) {
        rig.caps[0]->pushPcm(block.data(), block.size() * 2);
        EXPECT_TRUE(waitFor([&] { return list.written(id) == n * 4096; }));
        if (n == 16) EXPECT_TRUE(gate.waitEntered()) << "writer write deadlock";
    }
    rig.caps[1]->pushPcm(block.data(), block.size() * 2);
    EXPECT_TRUE(waitFor([&] { return list.written(sibling) == 4096; }));
    EXPECT_EQ(list.poll()[0].state, StreamState::Running);
    EXPECT_EQ(list.poll()[1].state, StreamState::Running);
    EXPECT_FALSE(rig.stopped[0]->load());
    EXPECT_FALSE(rig.stopped[1]->load());
    gate.release();
    const Result result = list.stopDump(id);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.message, "WavSink: ring overflow");
    WavReader reader;
    ASSERT_TRUE(reader.open(path));
    // One batch in the blocked writer plus the unchanged one-MiB sink ring.
    std::vector<int16_t> prefix((256u * 1024u + 1024u * 1024u) / 2u);
    EXPECT_EQ(reader.read(prefix.data(), prefix.size() * 2), prefix.size() * 2);
    EXPECT_EQ(prefix, std::vector<int16_t>(prefix.size(), 123));
    int16_t extra = 0;
    EXPECT_EQ(reader.read(&extra, sizeof(extra)), 0u);
    reader.close();
    list.destroyAll();
    DeleteFileW(path.c_str());
}

static void failAfterStartingSlowDump(const std::wstring& path, bool blockWrite) {
    ListRig rig;
    SlowOutputGate gate;
    gate.blockWrite = blockWrite;
    CaptureTrackList list(rig.factory(), {}, [&] { return std::make_unique<SlowOutput>(gate); });
    SlowOutputRelease release{gate};
    CaptureTrackCreate spec;
    spec.wavPath = path;
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    if (blockWrite) {
        const std::vector<int16_t> pcm(256u * 1024u / 2u, 123);
        rig.caps[0]->pushPcm(pcm.data(), pcm.size() * 2);
        ASSERT_TRUE(gate.waitEntered());
    }
    FAIL() << "injected early assertion";
}

TEST(CaptureTrackList, SlowDumpFatalAssertionDoesNotBlockTrackTeardown) {
    static const auto path = tempWav(L"early_assertion");
    EXPECT_FATAL_FAILURE(failAfterStartingSlowDump(path, false), "injected early assertion");
    DeleteFileW(path.c_str());
    EXPECT_FATAL_FAILURE(failAfterStartingSlowDump(path, true), "injected early assertion");
    DeleteFileW(path.c_str());
}
