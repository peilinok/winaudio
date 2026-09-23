#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "AppUiText.h"
#include "CaptureTrackList.h"
#include "RenderTrackList.h"
#include "RingBuffer.h"

using namespace wa;

namespace {

class FakeRender : public IAudioBackend {
public:
    explicit FakeRender(int* failOpens, int* failStarts, std::atomic<bool>* stopped,
                        RenderFill* fill = nullptr)
        : failOpens_(failOpens), failStarts_(failStarts), stopped_(stopped), fill_(fill) {}

    Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                const StreamParams& params) override {
        if (failOpens_ && *failOpens_ > 0) {
            --(*failOpens_);
            return Result::Fail(122, "fake: open failed");
        }
        openedId_ = id;
        openedFormat_ = fmt;
        openedParams_ = params;
        opened_ = true;
        ring_ = ring;
        channels_ = fmt.channels;
        return Result::Ok();
    }
    Result start() override {
        if (failStarts_ && *failStarts_ > 0) {
            --(*failStarts_);
            return Result::Fail(123, "fake: start failed");
        }
        started_ = true;
        return Result::Ok();
    }
    void stop() override {
        if (stopped_) stopped_->store(true, std::memory_order_relaxed);
    }
    void close() override {}
    BackendStats stats() const override {
        BackendStats s{};
        s.actualFormat = openedFormat_;
        return s;
    }

    std::vector<int16_t> pull(size_t frames) {
        if (!fill_ || !fill_->fn || channels_ == 0 || frames > 0xffffffffu) return {};
        std::vector<int16_t> out(frames * channels_, 0x7fff);
        fill_->fn(fill_->ctx, out.data(), static_cast<uint32_t>(frames));
        return out;
    }

    DeviceId openedId_;
    AudioFormat openedFormat_{};
    StreamParams openedParams_{};
    bool opened_ = false;
    bool started_ = false;
    RingBuffer* ring_ = nullptr;
    uint16_t channels_ = 0;

private:
    int* failOpens_ = nullptr;
    int* failStarts_ = nullptr;
    std::atomic<bool>* stopped_ = nullptr;
    RenderFill* fill_ = nullptr;
};

struct Rig {
    int failOpens = 0;
    int failStarts = 0;
    std::vector<FakeRender*> renders;
    std::vector<std::unique_ptr<std::atomic<bool>>> stopped;

    RenderTrackList::BackendFactory factory() {
        return [this](const DeviceId&, const AudioFormat&, RenderFill* fill) {
            stopped.push_back(std::make_unique<std::atomic<bool>>(false));
            auto backend = std::make_unique<FakeRender>(&failOpens, &failStarts,
                                                        stopped.back().get(), fill);
            renders.push_back(backend.get());
            return backend;
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

AudioFormat pcm(uint32_t rate, uint16_t bits, uint16_t channels, uint32_t mask, bool isFloat = false) {
    AudioFormat fmt;
    fmt.sampleRate = rate;
    fmt.bitsPerSample = bits;
    fmt.channels = channels;
    fmt.channelMask = mask;
    fmt.isFloat = isFloat;
    return fmt;
}

} // namespace

TEST(RenderTrackList, OpensWithNoTracks) {
    Rig rig;
    RenderTrackList list(rig.factory());
    EXPECT_TRUE(list.poll().empty());
}

TEST(RenderTrackList, CreateStartsSharedSilentTrack) {
    Rig rig;
    RenderTrackList list(rig.factory());
    RenderTrackCreate spec;
    spec.deviceId = L"spk-1";
    spec.mode = RenderLayoutMode::Layout;
    spec.source = pcm(96000, 24, 2, 0x3u);
    TrackId id = 0;
    Result r = list.create(spec, &id);
    ASSERT_TRUE(r) << r.message;
    EXPECT_NE(id, 0u);

    auto st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0].id, id);
    EXPECT_EQ(st[0].state, StreamState::Running);
    EXPECT_EQ(st[0].deviceId, L"spk-1");
    EXPECT_EQ(st[0].clientFormat.sampleRate, 48000u);
    EXPECT_EQ(st[0].clientFormat.bitsPerSample, 16u);
    EXPECT_FALSE(st[0].clientFormat.isFloat);
    EXPECT_EQ(st[0].clientFormat.channels, 2u);
    EXPECT_EQ(st[0].clientFormat.channelMask, 0x3u);
    EXPECT_EQ(st[0].actualFormat.sampleRate, 48000u);
    EXPECT_EQ(st[0].actualFormat.bitsPerSample, 16u);
    ASSERT_EQ(st[0].channelPaused.size(), 2u);
    EXPECT_EQ(st[0].channelPaused[0], 1u);
    EXPECT_EQ(st[0].channelPaused[1], 1u);

    ASSERT_EQ(rig.renders.size(), 1u);
    EXPECT_TRUE(rig.renders[0]->opened_);
    EXPECT_TRUE(rig.renders[0]->started_);
    EXPECT_EQ(rig.renders[0]->openedId_, L"spk-1");
    EXPECT_TRUE(rig.renders[0]->openedParams_.isDefault());
    EXPECT_EQ(rig.renders[0]->openedFormat_.sampleRate, 48000u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.bitsPerSample, 16u);
    EXPECT_FALSE(rig.renders[0]->openedFormat_.isFloat);
    EXPECT_EQ(rig.renders[0]->openedFormat_.channelMask, 0x3u);

    const std::vector<int16_t> frames = rig.renders[0]->pull(16);
    ASSERT_EQ(frames.size(), 32u);
    for (int16_t sample : frames) EXPECT_EQ(sample, 0);

    list.destroyAll();
}

TEST(RenderTrackList, SystemDefaultUsesMixChannelsAndMaskNotRate) {
    Rig rig;
    RenderTrackList list(rig.factory());
    RenderTrackCreate spec;
    spec.deviceId = L"mix-dev";
    spec.mode = RenderLayoutMode::SystemDefault;
    spec.source = pcm(96000, 32, 8, 0x63Fu, true);
    TrackId id = 0;
    ASSERT_TRUE(list.create(spec, &id));
    ASSERT_EQ(rig.renders.size(), 1u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.sampleRate, 48000u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.bitsPerSample, 16u);
    EXPECT_FALSE(rig.renders[0]->openedFormat_.isFloat);
    EXPECT_EQ(rig.renders[0]->openedFormat_.channels, 8u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.channelMask, 0x63Fu);
    auto st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0].actualFormat.sampleRate, 48000u);
    EXPECT_EQ(st[0].actualFormat.bitsPerSample, 16u);
    EXPECT_EQ(st[0].channelPaused.size(), 8u);
    list.destroyAll();
}

TEST(RenderTrackList, SystemDefaultKeepsAnEmptyMixMask) {
    Rig rig;
    RenderTrackList list(rig.factory());
    RenderTrackCreate spec;
    spec.mode = RenderLayoutMode::SystemDefault;
    spec.source = pcm(44100, 24, 2, 0u);
    ASSERT_TRUE(list.create(spec, nullptr));
    ASSERT_EQ(rig.renders.size(), 1u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.sampleRate, 48000u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.bitsPerSample, 16u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.channels, 2u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.channelMask, 0u);
    list.destroyAll();
}

TEST(RenderTrackList, EmptyMixStaysListedWithItsOwnError) {
    Rig rig;
    RenderTrackList list(rig.factory());
    RenderTrackCreate good;
    good.mode = RenderLayoutMode::Layout;
    good.source = pcm(48000, 16, 2, 0x3u);
    TrackId goodId = 0;
    ASSERT_TRUE(list.create(good, &goodId));

    RenderTrackCreate empty;
    empty.mode = RenderLayoutMode::SystemDefault;
    empty.source.channels = 0;
    empty.source.channelMask = 0;
    TrackId badId = 0;
    Result r = list.create(empty, &badId);
    EXPECT_FALSE(r);
    EXPECT_NE(badId, 0u);
    EXPECT_NE(badId, goodId);

    auto st = list.poll();
    ASSERT_EQ(st.size(), 2u);
    bool sawRun = false;
    bool sawErr = false;
    for (const auto& row : st) {
        if (row.id == goodId) {
            sawRun = row.state == StreamState::Running;
        }
        if (row.id == badId) {
            sawErr = row.state == StreamState::Error && row.message == "render mix has no channels";
        }
    }
    EXPECT_TRUE(sawRun);
    EXPECT_TRUE(sawErr);
    list.destroyAll();
}

TEST(RenderLayout, CustomIgnoresRateAndBitDepth) {
    struct Case {
        const char* text;
        bool ok;
        uint16_t channels;
        uint32_t mask;
    };
    const Case cases[] = {
        {"96000/24/1", true, 1, 0x4u},
        {"96000/24/2", true, 2, 0x3u},
        {"8000/8/3", true, 3, 0u},
        {"44100/16/4", true, 4, 0x33u},
        {"48000/32/5f", true, 5, 0u},
        {"96000/24/6", true, 6, 0x3Fu},
        {"192000/16/7", true, 7, 0u},
        {"22050/16/8", true, 8, 0x63Fu},
        {"48000/16/9", false, 0, 0u},
        {"48000/16/0", false, 0, 0u},
        {"nope", false, 0, 0u},
    };
    for (const Case& c : cases) {
        const RenderCustomLayout got = renderLayoutFromCustom(c.text);
        EXPECT_EQ(got.ok, c.ok) << c.text;
        if (!c.ok) {
            EXPECT_EQ(got.message, "invalid format") << c.text;
            continue;
        }
        EXPECT_EQ(got.channels, c.channels) << c.text;
        EXPECT_EQ(got.channelMask, c.mask) << c.text;
    }

    const RenderCustomLayout stereo = renderLayoutFromCustom("96000/32/2f");
    ASSERT_TRUE(stereo.ok);
    Rig rig;
    RenderTrackList list(rig.factory());
    RenderTrackCreate spec;
    spec.mode = RenderLayoutMode::Layout;
    spec.source = pcm(96000, 32, stereo.channels, stereo.channelMask, true);
    ASSERT_TRUE(list.create(spec, nullptr));
    ASSERT_EQ(rig.renders.size(), 1u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.sampleRate, 48000u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.bitsPerSample, 16u);
    EXPECT_FALSE(rig.renders[0]->openedFormat_.isFloat);
    EXPECT_EQ(rig.renders[0]->openedFormat_.channels, 2u);
    EXPECT_EQ(rig.renders[0]->openedFormat_.channelMask, 0x3u);
    list.destroyAll();
}

TEST(RenderLayout, CollapsesCandidatesThatDifferOnlyByRateOrDepth) {
    const std::vector<AudioFormat> candidates = {
        pcm(48000, 16, 8, 0x63Fu),
        pcm(96000, 24, 8, 0x63Fu),
        pcm(44100, 16, 2, 0u),
        pcm(192000, 32, 2, 0x3u, true),
        pcm(48000, 16, 2, 0x5u),
        pcm(48000, 16, 3, 0u),
        pcm(96000, 32, 3, 0u, true),
        pcm(48000, 16, 1, 0x4u),
        pcm(48000, 16, 1, 0x1u),
        pcm(48000, 16, 4, 0x33u),
        pcm(44100, 24, 4, 0x33u),
        pcm(48000, 16, 6, 0x3Fu),
    };
    const auto layouts = collapseSharedLayouts(candidates);
    ASSERT_EQ(layouts.size(), 8u);

    struct Expect {
        const char* label;
        uint16_t channels;
        uint32_t mask;
    };
    const Expect expected[] = {
        {"7.1 / mask 0x63F", 8, 0x63Fu},
        {"Stereo / mask 0x3", 2, 0x3u},
        {"mask 0x5", 2, 0x5u},
        {"3 channels", 3, 0u},
        {"Mono / mask 0x4", 1, 0x4u},
        {"mask 0x1", 1, 0x1u},
        {"Quad / mask 0x33", 4, 0x33u},
        {"5.1 / mask 0x3F", 6, 0x3Fu},
    };
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(layouts[i].label, expected[i].label) << i;
        EXPECT_EQ(layouts[i].channels, expected[i].channels) << i;
        EXPECT_EQ(layouts[i].channelMask, expected[i].mask) << i;
        EXPECT_EQ(layouts[i].label.find("48000"), std::string::npos) << layouts[i].label;
        EXPECT_EQ(layouts[i].label.find("96000"), std::string::npos) << layouts[i].label;
        EXPECT_EQ(layouts[i].label.find("Hz"), std::string::npos) << layouts[i].label;
    }
}

TEST(RenderTrackList, FailedOpenStaysListedAndKeepsSibling) {
    Rig rig;
    RenderTrackList list(rig.factory());
    RenderTrackCreate spec;
    spec.mode = RenderLayoutMode::Layout;
    spec.source = pcm(48000, 16, 2, 0x3u);
    TrackId goodId = 0;
    ASSERT_TRUE(list.create(spec, &goodId));

    rig.failOpens = 1;
    TrackId badId = 0;
    Result r = list.create(spec, &badId);
    EXPECT_FALSE(r);
    EXPECT_NE(badId, goodId);

    auto st = list.poll();
    ASSERT_EQ(st.size(), 2u);
    bool sawRun = false;
    bool sawErr = false;
    for (const auto& row : st) {
        if (row.id == goodId && row.state == StreamState::Running) sawRun = true;
        if (row.id == badId && row.state == StreamState::Error &&
            row.message == "fake: open failed")
            sawErr = true;
    }
    EXPECT_TRUE(sawRun);
    EXPECT_TRUE(sawErr);
    list.destroyAll();
}

TEST(RenderTrackList, FailedStartStaysListedAndKeepsSibling) {
    Rig rig;
    RenderTrackList list(rig.factory());
    RenderTrackCreate spec;
    spec.mode = RenderLayoutMode::Layout;
    spec.source = pcm(48000, 16, 2, 0x3u);
    TrackId goodId = 0;
    ASSERT_TRUE(list.create(spec, &goodId));

    rig.failStarts = 1;
    TrackId badId = 0;
    Result r = list.create(spec, &badId);
    EXPECT_FALSE(r);

    auto st = list.poll();
    ASSERT_EQ(st.size(), 2u);
    bool sawRun = false;
    bool sawErr = false;
    for (const auto& row : st) {
        if (row.id == goodId && row.state == StreamState::Running) sawRun = true;
        if (row.id == badId && row.state == StreamState::Error &&
            row.message == "fake: start failed")
            sawErr = true;
    }
    EXPECT_TRUE(sawRun);
    EXPECT_TRUE(sawErr);
    list.destroyAll();
}

TEST(RenderTrackList, DestroyRemovesOneTrackAndStopsItsBackend) {
    Rig rig;
    RenderTrackList list(rig.factory());
    RenderTrackCreate spec;
    spec.mode = RenderLayoutMode::Layout;
    spec.deviceId = L"same";
    spec.source = pcm(48000, 16, 2, 0x3u);
    TrackId a = 0;
    TrackId b = 0;
    ASSERT_TRUE(list.create(spec, &a));
    ASSERT_TRUE(list.create(spec, &b));
    list.destroy(a);
    auto st = list.poll();
    ASSERT_EQ(st.size(), 1u);
    EXPECT_EQ(st[0].id, b);
    EXPECT_EQ(st[0].state, StreamState::Running);
    ASSERT_EQ(rig.stopped.size(), 2u);
    EXPECT_TRUE(rig.stopped[0]->load(std::memory_order_relaxed));
    EXPECT_FALSE(rig.stopped[1]->load(std::memory_order_relaxed));
    list.destroyAll();
}

TEST(RenderTrackList, DestroyAllDoesNotRemoveCaptureTracks) {
    Rig rig;
    RenderTrackList renders(rig.factory());
    RenderTrackCreate spec;
    spec.mode = RenderLayoutMode::Layout;
    spec.source = pcm(48000, 16, 2, 0x3u);
    ASSERT_TRUE(renders.create(spec, nullptr));

    CaptureTrackList captures([](const CaptureSource&, const AudioFormat*) {
        return std::unique_ptr<IAudioBackend>(
            std::make_unique<FakeRender>(nullptr, nullptr, nullptr));
    });
    ASSERT_TRUE(captures.create(CaptureTrackCreate{}, nullptr));
    EXPECT_EQ(captures.poll().size(), 1u);

    renders.destroyAll();
    EXPECT_TRUE(renders.poll().empty());
    EXPECT_EQ(captures.poll().size(), 1u);
    captures.destroyAll();
}

TEST(RenderTrackList, ThrowingFactoryStaysListedAndKeepsSibling) {
    int calls = 0;
    std::vector<std::unique_ptr<std::atomic<bool>>> stopped;
    RenderTrackList list([&](const DeviceId&, const AudioFormat&,
                             RenderFill*) -> std::unique_ptr<IAudioBackend> {
        if (++calls >= 2) throw std::runtime_error("factory blew up");
        stopped.push_back(std::make_unique<std::atomic<bool>>(false));
        return std::make_unique<FakeRender>(nullptr, nullptr, stopped.back().get());
    });
    RenderTrackCreate spec;
    spec.mode = RenderLayoutMode::Layout;
    spec.source = pcm(48000, 16, 2, 0x3u);
    TrackId goodId = 0;
    ASSERT_TRUE(list.create(spec, &goodId));
    TrackId badId = 0;
    Result r = list.create(spec, &badId);
    EXPECT_FALSE(r);
    EXPECT_NE(badId, goodId);
    auto st = list.poll();
    ASSERT_EQ(st.size(), 2u);
    bool sawRun = false;
    bool sawErr = false;
    for (const auto& row : st) {
        if (row.id == goodId && row.state == StreamState::Running) sawRun = true;
        if (row.id == badId && row.state == StreamState::Error &&
            row.message == "factory blew up")
            sawErr = true;
    }
    EXPECT_TRUE(sawRun);
    EXPECT_TRUE(sawErr);
    list.destroyAll();
}

TrackId startLayout(RenderTrackList& list, uint16_t channels, uint32_t mask) {
    RenderTrackCreate spec;
    spec.mode = RenderLayoutMode::Layout;
    spec.deviceId = L"spk";
    spec.source = pcm(48000, 16, channels, mask);
    TrackId id = 0;
    EXPECT_TRUE(list.create(spec, &id));
    return id;
}

int16_t at(const std::vector<int16_t>& frames, uint16_t channels, size_t frame, uint16_t channel) {
    return frames[frame * channels + channel];
}

TEST(ChannelPhrase, UnmaskedSlotsAreChannelNThroughEightOnly) {
    EXPECT_EQ(phraseForSlot(0, 0), ChannelPhrase::Channel1);
    EXPECT_EQ(phraseForSlot(0, 7), ChannelPhrase::Channel8);
    EXPECT_EQ(phraseForSlot(0, 8), ChannelPhrase::None);
}

TEST(ChannelPhrase, SlotFollowsTheLowestSetMaskBit) {
    const uint32_t stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    EXPECT_EQ(phraseForSlot(stereo, 0), ChannelPhrase::FrontLeft);
    EXPECT_EQ(phraseForSlot(stereo, 1), ChannelPhrase::FrontRight);
    const uint32_t nine = defaultChannelMask(8) | SPEAKER_TOP_FRONT_LEFT;
    EXPECT_EQ(phraseForSlot(nine, 8), ChannelPhrase::TopFrontLeft);
}

TEST(RenderTrackList, ContinueLoopsPhraseOnThatChannelOnly) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::FrontLeft, {1000, 2000, 3000});
    catalog.set(ChannelPhrase::FrontRight, {4000, 5000, 6000});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    ASSERT_TRUE(list.continueChannel(id, 0));

    const auto status = list.poll();
    ASSERT_EQ(status.size(), 1u);
    EXPECT_EQ(status[0].state, StreamState::Running);
    EXPECT_EQ(status[0].channelPaused[0], 0u);
    EXPECT_EQ(status[0].channelPaused[1], 1u);

    const std::vector<int16_t> frames = rig.renders[0]->pull(6);
    ASSERT_EQ(frames.size(), 12u);
    const int16_t left[] = {1000, 2000, 3000, 1000, 2000, 3000};
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(at(frames, 2, static_cast<size_t>(i), 0), left[i]) << i;
        EXPECT_EQ(at(frames, 2, static_cast<size_t>(i), 1), 0) << i;
    }
    list.destroyAll();
}

TEST(RenderTrackList, PauseReturnsSilenceAndKeepsTheTrack) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::FrontLeft, {1000, 2000, 3000});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 1, SPEAKER_FRONT_LEFT);
    ASSERT_TRUE(list.continueChannel(id, 0));
    const std::vector<int16_t> first = rig.renders[0]->pull(1);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0], 1000);

    ASSERT_TRUE(list.pauseChannel(id, 0));
    const std::vector<int16_t> silenced = rig.renders[0]->pull(2);
    EXPECT_EQ(silenced[0], 0);
    EXPECT_EQ(silenced[1], 0);
    const auto status = list.poll();
    ASSERT_EQ(status.size(), 1u);
    EXPECT_EQ(status[0].id, id);
    EXPECT_EQ(status[0].state, StreamState::Running);
    EXPECT_EQ(status[0].channelPaused[0], 1u);

    ASSERT_TRUE(list.continueChannel(id, 0));
    const std::vector<int16_t> resumed = rig.renders[0]->pull(1);
    EXPECT_EQ(resumed[0], 2000);
    list.destroyAll();
}

TEST(RenderTrackList, PlayOnceFromPauseStaysPaused) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::FrontLeft, {1000, 2000, 3000});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 1, SPEAKER_FRONT_LEFT);
    ASSERT_TRUE(list.playOnce(id, 0));
    EXPECT_EQ(list.poll()[0].channelPaused[0], 1u);

    const std::vector<int16_t> once = rig.renders[0]->pull(3);
    EXPECT_EQ(once[0], 1000);
    EXPECT_EQ(once[1], 2000);
    EXPECT_EQ(once[2], 3000);
    const std::vector<int16_t> after = rig.renders[0]->pull(2);
    EXPECT_EQ(after[0], 0);
    EXPECT_EQ(after[1], 0);
    EXPECT_EQ(list.poll()[0].channelPaused[0], 1u);
    EXPECT_EQ(list.poll()[0].state, StreamState::Running);
    list.destroyAll();
}

TEST(RenderTrackList, PlayOnceFromContinueRestartsWithoutASecondCopy) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::FrontLeft, {1000, 2000, 3000});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 1, SPEAKER_FRONT_LEFT);
    ASSERT_TRUE(list.continueChannel(id, 0));
    EXPECT_EQ(rig.renders[0]->pull(1)[0], 1000);

    ASSERT_TRUE(list.playOnce(id, 0));
    EXPECT_EQ(list.poll()[0].channelPaused[0], 0u);
    const std::vector<int16_t> restarted = rig.renders[0]->pull(4);
    EXPECT_EQ(restarted[0], 1000);
    EXPECT_EQ(restarted[1], 2000);
    EXPECT_EQ(restarted[2], 3000);
    EXPECT_EQ(restarted[3], 1000);
    list.destroyAll();
}

TEST(RenderTrackList, VolumeScalesSamplesAndZeroStaysContinuing) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::FrontLeft, {1000, 2000, 3000, 4000});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 1, SPEAKER_FRONT_LEFT);
    ASSERT_TRUE(list.continueChannel(id, 0));
    ASSERT_TRUE(list.setVolume(id, 0, 50));
    EXPECT_EQ(rig.renders[0]->pull(1)[0], 500);

    ASSERT_TRUE(list.setVolume(id, 0, 0));
    EXPECT_EQ(rig.renders[0]->pull(1)[0], 0);
    EXPECT_EQ(list.poll()[0].channelPaused[0], 0u);
    EXPECT_EQ(list.poll()[0].channels[0].volumePercent, 0u);

    ASSERT_TRUE(list.setVolume(id, 0, 100));
    EXPECT_EQ(rig.renders[0]->pull(1)[0], 3000);
    ASSERT_TRUE(list.playOnce(id, 0));
    EXPECT_EQ(rig.renders[0]->pull(1)[0], 1000);
    list.destroyAll();
}

TEST(RenderTrackList, VolumeSurvivesPause) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::FrontLeft, {1000});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 1, SPEAKER_FRONT_LEFT);
    ASSERT_TRUE(list.setVolume(id, 0, 40));
    ASSERT_TRUE(list.pauseChannel(id, 0));
    EXPECT_EQ(list.poll()[0].channels[0].volumePercent, 40u);
    EXPECT_EQ(list.poll()[0].channelPaused[0], 1u);
    ASSERT_TRUE(list.continueChannel(id, 0));
    EXPECT_EQ(rig.renders[0]->pull(1)[0], 400);
    list.destroyAll();
}

TEST(RenderTrackList, UnmaskedChannelsPlayChannelNFromOne) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::Channel1, {11, 12});
    catalog.set(ChannelPhrase::Channel2, {21, 22});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 2, 0);
    ASSERT_TRUE(list.continueChannel(id, 0));
    ASSERT_TRUE(list.continueChannel(id, 1));
    const std::vector<int16_t> frames = rig.renders[0]->pull(2);
    EXPECT_EQ(at(frames, 2, 0, 0), 11);
    EXPECT_EQ(at(frames, 2, 0, 1), 21);
    EXPECT_EQ(at(frames, 2, 1, 0), 12);
    EXPECT_EQ(at(frames, 2, 1, 1), 22);
    EXPECT_EQ(list.poll()[0].channels[0].phrase, ChannelPhrase::Channel1);
    EXPECT_EQ(list.poll()[0].channels[1].phrase, ChannelPhrase::Channel2);
    list.destroyAll();
}

TEST(RenderTrackList, UnmaskedNinthChannelHasNoPhraseAndStaysSilent) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::Channel1, {11, 12});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 9, 0);
    ASSERT_TRUE(list.continueChannel(id, 8));
    ASSERT_TRUE(list.playOnce(id, 8));
    ASSERT_TRUE(list.setVolume(id, 8, 100));
    const std::vector<int16_t> frames = rig.renders[0]->pull(2);
    EXPECT_EQ(at(frames, 9, 0, 8), 0);
    EXPECT_EQ(at(frames, 9, 1, 8), 0);
    const auto status = list.poll();
    EXPECT_EQ(status[0].channels[8].phrase, ChannelPhrase::None);
    EXPECT_EQ(status[0].channels[8].hasPhrase, 0u);
    EXPECT_EQ(status[0].channels[0].hasPhrase, 1u);
    EXPECT_EQ(status[0].chartChannels, 8u);
    EXPECT_EQ(list.tapChannels(id), 8u);
    float ignored = 0.f;
    EXPECT_FALSE(list.snapshotChannelEndingAt(id, 8, list.written(id), 1, &ignored));
    list.destroyAll();
}

TEST(RenderTrackList, PositionedChannelPastEightKeepsItsRolePhrase) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::TopFrontLeft, {9, 8, 7});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const uint32_t mask = defaultChannelMask(8) | SPEAKER_TOP_FRONT_LEFT;
    const TrackId id = startLayout(list, 9, mask);
    ASSERT_TRUE(list.continueChannel(id, 8));
    const std::vector<int16_t> frames = rig.renders[0]->pull(3);
    EXPECT_EQ(at(frames, 9, 0, 8), 9);
    EXPECT_EQ(at(frames, 9, 1, 8), 8);
    EXPECT_EQ(at(frames, 9, 2, 8), 7);
    EXPECT_EQ(at(frames, 9, 0, 0), 0);
    EXPECT_EQ(list.poll()[0].channels[8].phrase, ChannelPhrase::TopFrontLeft);
    EXPECT_EQ(list.poll()[0].channels[8].hasPhrase, 1u);
    list.destroyAll();
}

TEST(RenderTrackList, ScopeTapMatchesPlayedSamplesIncludingVolume) {
    PhraseCatalog catalog;
    catalog.set(ChannelPhrase::FrontLeft, {1000, -2000});
    Rig rig;
    RenderTrackList list(rig.factory(), &catalog);
    const TrackId id = startLayout(list, 1, SPEAKER_FRONT_LEFT);
    ASSERT_TRUE(list.setVolume(id, 0, 50));
    ASSERT_TRUE(list.continueChannel(id, 0));
    ASSERT_EQ(rig.renders[0]->pull(2).size(), 2u);
    const uint64_t end = list.written(id);
    ASSERT_GE(end, 2u);
    float out[2] = {1.f, 1.f};
    ASSERT_TRUE(list.snapshotChannelEndingAt(id, 0, end, 2, out));
    EXPECT_FLOAT_EQ(out[0], 500.f / 32768.f);
    EXPECT_FLOAT_EQ(out[1], -1000.f / 32768.f);
    list.destroyAll();
}

TEST(RenderStrings, RenderPageWords) {
    EXPECT_STREQ(ui_text::kRenderTab, "Render");
    EXPECT_STREQ(ui_text::kRenderEmptyHint, "Create a Track to play to a render endpoint.");
    EXPECT_STREQ(ui_text::kRenderEndpoint, "Render endpoint");
    EXPECT_STREQ(ui_text::kRenderLayout, "Layout");
    EXPECT_STREQ(ui_text::kRenderCustomIgnored, "Sample rate and bit depth are ignored.");
    EXPECT_STREQ(ui_text::kRenderChannelsPaused, "All channels paused");
    EXPECT_STREQ(ui_text::kRenderPause, "Pause");
    EXPECT_STREQ(ui_text::kRenderContinue, "Continue");
    EXPECT_STREQ(ui_text::kRenderPlayOnce, "Play once");
    EXPECT_STREQ(ui_text::kRenderNoPhrase, "No phrase");
    EXPECT_STREQ(ui_text::kRenderVolume, "Volume");
    EXPECT_STREQ(ui_text::channelPhraseText(ChannelPhrase::Channel1), "channel 1");
    EXPECT_STREQ(ui_text::channelPhraseText(ChannelPhrase::Channel8), "channel 8");
    EXPECT_STREQ(ui_text::channelPhraseText(ChannelPhrase::None), "No phrase");
    EXPECT_STREQ(ui_text::channelPhraseText(ChannelPhrase::TopFrontLeft), "Top front left");
}

