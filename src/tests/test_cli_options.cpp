#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <vector>
#include "CaptureTrackList.h"
#include "CliOptions.h"
#include "RingBuffer.h"

TEST(CliOptions, CompatSingleOutIsOneEndpointTrack) {
    const wchar_t* argv[] = {L"WinAudioCli", L"capture", L"--out", L"a.wav", L"--device", L"mic"};
    auto g = wa::cli::parseCaptureGroup(6, const_cast<wchar_t**>(argv));
    ASSERT_TRUE(g.ok) << g.message;
    ASSERT_EQ(g.tracks.size(), 1u);
    EXPECT_EQ(g.tracks[0].out, L"a.wav");
    EXPECT_EQ(g.tracks[0].device, L"mic");
    EXPECT_FALSE(g.tracks[0].loopback);
    EXPECT_FALSE(g.tracks[0].hasPid);
    auto spec = wa::cli::captureCreateFromSegment(g.tracks[0], g.backend);
    EXPECT_EQ(spec.source.kind, wa::CaptureSourceKind::Endpoint);
    EXPECT_EQ(spec.wavPath, L"a.wav");
}

TEST(CliOptions, RepeatableTrackSegmentsMixKinds) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"capture", L"--seconds", L"3",
        L"--track", L"--out", L"a.wav", L"--loopback", L"--device", L"spk",
        L"--track", L"--out", L"b.wav", L"--pid", L"4242", L"--exclude-tree",
        L"--track", L"--out", L"c.wav"
    };
    auto g = wa::cli::parseCaptureGroup(19, const_cast<wchar_t**>(argv));
    ASSERT_TRUE(g.ok) << g.message;
    ASSERT_EQ(g.tracks.size(), 3u);
    EXPECT_EQ(g.seconds, 3);
    EXPECT_TRUE(g.tracks[0].loopback);
    EXPECT_EQ(g.tracks[0].device, L"spk");
    EXPECT_TRUE(g.tracks[1].hasPid);
    EXPECT_EQ(g.tracks[1].pid, 4242u);
    EXPECT_TRUE(g.tracks[1].excludeTree);
    EXPECT_FALSE(g.tracks[2].loopback);
    EXPECT_FALSE(g.tracks[2].hasPid);

    auto a = wa::cli::captureCreateFromSegment(g.tracks[0], g.backend);
    auto b = wa::cli::captureCreateFromSegment(g.tracks[1], g.backend);
    auto c = wa::cli::captureCreateFromSegment(g.tracks[2], g.backend);
    EXPECT_EQ(a.source.kind, wa::CaptureSourceKind::SystemLoopback);
    EXPECT_EQ(b.source.kind, wa::CaptureSourceKind::ApplicationLoopback);
    EXPECT_EQ(b.source.processLoopbackMode, wa::ProcessLoopbackMode::ExcludeTree);
    EXPECT_EQ(c.source.kind, wa::CaptureSourceKind::Endpoint);
}

TEST(CliOptions, PidAndLoopbackInOneTrackIsUsageError) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"capture", L"--track", L"--out", L"a.wav",
        L"--pid", L"8", L"--loopback"
    };
    auto g = wa::cli::parseCaptureGroup(8, const_cast<wchar_t**>(argv));
    EXPECT_FALSE(g.ok);
    EXPECT_TRUE(g.tracks.empty());
}

TEST(CliOptions, TrackWithoutOutIsUsageError) {
    const wchar_t* argv[] = {L"WinAudioCli", L"capture", L"--track", L"--loopback"};
    auto g = wa::cli::parseCaptureGroup(4, const_cast<wchar_t**>(argv));
    EXPECT_FALSE(g.ok);
}

TEST(CliOptions, PerSegmentFormatAndSilentRender) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"capture", L"--backend", L"wasapi-shared",
        L"--track", L"--out", L"a.wav", L"--loopback", L"--format", L"48000/16/2",
        L"--track", L"--out", L"b.wav", L"--loopback", L"--no-silent-render"
    };
    auto g = wa::cli::parseCaptureGroup(15, const_cast<wchar_t**>(argv));
    ASSERT_TRUE(g.ok) << g.message;
    EXPECT_EQ(g.backend, wa::BackendKind::WasapiShared);
    ASSERT_EQ(g.tracks.size(), 2u);
    EXPECT_TRUE(g.tracks[0].hasFormat);
    EXPECT_EQ(g.tracks[0].format.sampleRate, 48000u);
    EXPECT_FALSE(g.tracks[0].noSilentRender);
    EXPECT_TRUE(g.tracks[1].noSilentRender);
    EXPECT_FALSE(g.tracks[1].hasFormat);
    auto a = wa::cli::captureCreateFromSegment(g.tracks[0], g.backend);
    auto b = wa::cli::captureCreateFromSegment(g.tracks[1], g.backend);
    EXPECT_TRUE(a.loopbackOptions.silentRender);
    EXPECT_FALSE(b.loopbackOptions.silentRender);
    ASSERT_NE(a.requested, nullptr);
    EXPECT_EQ(a.requested->sampleRate, 48000u);
}

TEST(CliOptions, EqualRecipesAllowedByParser) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"capture",
        L"--track", L"--out", L"a.wav", L"--loopback", L"--device", L"spk",
        L"--track", L"--out", L"b.wav", L"--loopback", L"--device", L"spk"
    };
    auto g = wa::cli::parseCaptureGroup(14, const_cast<wchar_t**>(argv));
    ASSERT_TRUE(g.ok) << g.message;
    ASSERT_EQ(g.tracks.size(), 2u);
    EXPECT_EQ(g.tracks[0].device, g.tracks[1].device);
}

TEST(CliOptions, RejectsZeroPid) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"capture", L"--track", L"--out", L"a.wav", L"--pid", L"0"
    };
    auto g = wa::cli::parseCaptureGroup(7, const_cast<wchar_t**>(argv));
    EXPECT_FALSE(g.ok);
}

TEST(CliOptions, LoopbackSilentRenderDefaultsEnabled) {
    const wchar_t* argv[] = {L"WinAudioCli", L"capture", L"--loopback"};

    wa::LoopbackOptions opts =
        wa::cli::parseLoopbackOptions(3, const_cast<wchar_t**>(argv));

    EXPECT_TRUE(opts.silentRender);
}

TEST(CliOptions, NoSilentRenderDisablesHelper) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"monitor", L"--loopback", L"--no-silent-render"
    };

    wa::LoopbackOptions opts =
        wa::cli::parseLoopbackOptions(4, const_cast<wchar_t**>(argv));

    EXPECT_FALSE(opts.silentRender);
}

namespace {

class FakeCap : public wa::IAudioBackend {
public:
    explicit FakeCap(std::atomic<bool>* stopped) : stopped_(stopped) {}
    wa::Result open(const wa::DeviceId&, const wa::AudioFormat&, wa::RingBuffer*,
                    const wa::StreamParams&) override {
        return wa::Result::Ok();
    }
    wa::Result start() override { return wa::Result::Ok(); }
    void stop() override {
        if (stopped_) stopped_->store(true);
    }
    void close() override {}
    wa::BackendStats stats() const override {
        wa::BackendStats s{};
        s.actualFormat = {48000, 2, 16, false};
        return s;
    }
    std::atomic<bool>* stopped_ = nullptr;
};

} // namespace

TEST(CliOptions, ParsedGroupCreatesIndependentTracks) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"capture",
        L"--track", L"--out", L"?:\\bad_a.wav", L"--loopback",
        L"--track", L"--out", L"?:\\bad_b.wav", L"--pid", L"99"
    };
    auto g = wa::cli::parseCaptureGroup(11, const_cast<wchar_t**>(argv));
    ASSERT_TRUE(g.ok) << g.message;
    ASSERT_EQ(g.tracks.size(), 2u);

    std::atomic<bool> stop0{false};
    std::atomic<bool> stop1{false};
    int n = 0;
    wa::CaptureTrackList list(
        [&](const wa::CaptureSource&, const wa::AudioFormat*) {
            auto* flag = (n++ == 0) ? &stop0 : &stop1;
            return std::unique_ptr<wa::IAudioBackend>(new FakeCap(flag));
        });

    bool anyFail = false;
    for (const auto& seg : g.tracks) {
        auto spec = wa::cli::captureCreateFromSegment(seg, g.backend);
        if (!list.create(spec, nullptr)) anyFail = true;
    }
    EXPECT_FALSE(anyFail);
    EXPECT_EQ(list.poll().size(), 2u);

    ASSERT_TRUE([&] {
        for (int i = 0; i < 200; ++i) {
            auto st = list.poll();
            if (st.size() == 2
                && st[0].state == wa::StreamState::Error
                && st[1].state == wa::StreamState::Error)
                return true;
            Sleep(5);
        }
        return false;
    }());

    list.destroyAll();
    EXPECT_TRUE(list.poll().empty());
    EXPECT_TRUE(stop0.load());
    EXPECT_TRUE(stop1.load());
}
