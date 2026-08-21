#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "CaptureTrackList.h"
#include "RingBuffer.h"
#include "TrackScopeReader.h"
#include "WavFile.h"

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
        return s;
    }

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
