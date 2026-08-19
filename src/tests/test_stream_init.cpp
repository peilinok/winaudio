#include <gtest/gtest.h>
#include "StreamInit.h"
#include "AudioFormat.h"
#include <objbase.h>
#include <vector>

using namespace wa;

namespace {

constexpr REFERENCE_TIME kSharedDefaultDuration = 1'000'000; // 100 ms in 100-ns units
constexpr REFERENCE_TIME kExclusiveMinPeriod = 100'000;      // 10 ms in 100-ns units

class FakeAudioClientInit : public AudioClientInit {
public:
    AudioFormat mixFormat{48000, 2, 16, false};
    HRESULT mixHr = S_OK;
    HRESULT initHr = S_OK;
    HRESULT rebuildHr = S_OK;
    std::vector<AudioFormat> supportedFormats;
    REFERENCE_TIME minPeriod = kExclusiveMinPeriod;
    REFERENCE_TIME deviceDefaultPeriod = kExclusiveMinPeriod;
    UINT32 alignedFrames = 480;
    bool failFirstInitWithNotAligned = false;

    AUDCLNT_SHAREMODE lastShareMode = AUDCLNT_SHAREMODE_SHARED;
    DWORD lastFlags = 0;
    REFERENCE_TIME lastDuration = 0;
    REFERENCE_TIME lastPeriodicity = -1;
    AudioFormat lastFormat{};
    int initializeCount = 0;
    int rebuildCount = 0;
    std::vector<AudioFormat> probedFormats;

    HRESULT getMixFormat(WAVEFORMATEX** mix) override {
        if (FAILED(mixHr)) return mixHr;
        auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (!wfx) return E_OUTOFMEMORY;
        ZeroMemory(wfx, sizeof(*wfx));
        wfx->wFormatTag = WAVE_FORMAT_PCM;
        wfx->nChannels = mixFormat.channels;
        wfx->nSamplesPerSec = mixFormat.sampleRate;
        wfx->wBitsPerSample = mixFormat.bitsPerSample;
        wfx->nBlockAlign = static_cast<WORD>(mixFormat.blockAlign());
        wfx->nAvgBytesPerSec = mixFormat.avgBytesPerSec();
        *mix = wfx;
        return S_OK;
    }

    HRESULT initialize(AUDCLNT_SHAREMODE shareMode, DWORD streamFlags,
                       REFERENCE_TIME bufferDuration, REFERENCE_TIME periodicity,
                       const WAVEFORMATEX* format) override {
        ++initializeCount;
        lastShareMode = shareMode;
        lastFlags = streamFlags;
        lastDuration = bufferDuration;
        lastPeriodicity = periodicity;
        if (format) lastFormat = fromWaveFormat(format);
        if (failFirstInitWithNotAligned && initializeCount == 1)
            return AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED;
        return initHr;
    }

    HRESULT isFormatSupported(AUDCLNT_SHAREMODE /*shareMode*/,
                              const WAVEFORMATEX* format) override {
        if (!format) return E_POINTER;
        AudioFormat f = fromWaveFormat(format);
        probedFormats.push_back(f);
        for (const auto& s : supportedFormats) {
            if (s == f) return S_OK;
        }
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    HRESULT getDevicePeriod(REFERENCE_TIME* defaultPeriod,
                            REFERENCE_TIME* minimumPeriod) override {
        if (defaultPeriod) *defaultPeriod = deviceDefaultPeriod;
        if (minimumPeriod) *minimumPeriod = minPeriod;
        return S_OK;
    }

    HRESULT getBufferSize(UINT32* frames) override {
        if (frames) *frames = alignedFrames;
        return S_OK;
    }

    HRESULT rebuild() override {
        ++rebuildCount;
        return rebuildHr;
    }
};

} // namespace

TEST(StreamInitShared, MixDefaultUsesEventCallbackOnly) {
    FakeAudioClientInit fake;
    StreamInitRequest req;
    StreamInitOutcome out;
    Result r = streamInitShared(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_SHARED);
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK));
    EXPECT_EQ(fake.lastDuration, kSharedDefaultDuration);
    EXPECT_EQ(fake.lastPeriodicity, 0);
    EXPECT_EQ(out.actualFormat, fake.mixFormat);
    EXPECT_EQ(out.frameBytes, fake.mixFormat.blockAlign());
    EXPECT_EQ(fake.lastFormat, out.actualFormat);
}

TEST(StreamInitShared, RequestedDefaultAddsAutoConvertAndSrc) {
    FakeAudioClientInit fake;
    AudioFormat want{44100, 2, 16, false};
    StreamInitRequest req;
    req.requested = &want;
    StreamInitOutcome out;
    Result r = streamInitShared(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_SHARED);
    const DWORD expected = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    EXPECT_EQ(fake.lastFlags, expected);
    EXPECT_EQ(out.actualFormat, want);
    EXPECT_EQ(out.frameBytes, want.blockAlign());
    EXPECT_EQ(fake.lastFormat, want);
}

TEST(StreamInitShared, CallerLoopbackExtraAppearsInFlags) {
    FakeAudioClientInit fake;
    StreamInitRequest req;
    req.extraFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
    StreamInitOutcome out;
    Result r = streamInitShared(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                 AUDCLNT_STREAMFLAGS_LOOPBACK));
}

TEST(StreamInitShared, RequestedPlusLoopbackExtraORsAutoConvert) {
    FakeAudioClientInit fake;
    AudioFormat want{44100, 2, 16, false};
    StreamInitRequest req;
    req.requested = &want;
    req.extraFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
    StreamInitOutcome out;
    Result r = streamInitShared(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    const DWORD expected = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                           AUDCLNT_STREAMFLAGS_LOOPBACK;
    EXPECT_EQ(fake.lastFlags, expected);
    EXPECT_EQ(out.actualFormat, want);
}

TEST(StreamInitShared, RequestedOffOmitsAutoConvert) {
    FakeAudioClientInit fake;
    AudioFormat want{44100, 2, 16, false};
    StreamInitRequest req;
    req.requested = &want;
    req.params.autoConvert = AutoConvert::Off;
    StreamInitOutcome out;
    Result r = streamInitShared(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_SHARED);
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK));
    EXPECT_EQ(out.actualFormat, want);
    EXPECT_EQ(fake.lastFormat, want);
}

TEST(StreamInitShared, MixForceAddsAutoConvertAndSrc) {
    FakeAudioClientInit fake;
    StreamInitRequest req;
    req.params.autoConvert = AutoConvert::Force;
    StreamInitOutcome out;
    Result r = streamInitShared(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_SHARED);
    const DWORD expected = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    EXPECT_EQ(fake.lastFlags, expected);
    EXPECT_EQ(out.actualFormat, fake.mixFormat);
    EXPECT_EQ(fake.lastFormat, fake.mixFormat);
}

TEST(StreamInitShared, BufferMsSetsDuration) {
    FakeAudioClientInit fake;
    StreamInitRequest req;
    req.params.bufferMs = 50;
    StreamInitOutcome out;
    Result r = streamInitShared(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastDuration, static_cast<REFERENCE_TIME>(50) * 10'000);
    EXPECT_EQ(fake.lastPeriodicity, 0);
}

TEST(StreamInitExclusive, RequestedSupportedUsesEventCallbackAndEqualPeriod) {
    FakeAudioClientInit fake;
    AudioFormat want{48000, 2, 16, false};
    fake.supportedFormats.push_back(want);
    StreamInitRequest req;
    req.requested = &want;
    req.direction = StreamInitDirection::Render;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_EXCLUSIVE);
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK));
    EXPECT_EQ(fake.lastDuration, kExclusiveMinPeriod);
    EXPECT_EQ(fake.lastPeriodicity, kExclusiveMinPeriod);
    EXPECT_EQ(out.actualFormat, want);
    EXPECT_EQ(out.frameBytes, want.blockAlign());
    EXPECT_EQ(fake.lastFormat, want);
    EXPECT_EQ(fake.initializeCount, 1);
}

TEST(StreamInitExclusive, RenderWithoutRequestedFails) {
    FakeAudioClientInit fake;
    StreamInitRequest req;
    req.direction = StreamInitDirection::Render;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(fake.initializeCount, 0);
}

TEST(StreamInitExclusive, CaptureWithoutRequestedUsesDefaultCandidates) {
    FakeAudioClientInit fake;
    const AudioFormat second{44100, 2, 16, false};
    fake.supportedFormats.push_back(second); // skip 48000/2/16, take next candidate
    StreamInitRequest req;
    req.direction = StreamInitDirection::Capture;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(out.actualFormat, second);
    EXPECT_EQ(fake.lastFormat, second);
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_EXCLUSIVE);
    ASSERT_GE(fake.probedFormats.size(), 2u);
    EXPECT_EQ(fake.probedFormats[0], (AudioFormat{48000, 2, 16, false}));
    EXPECT_EQ(fake.probedFormats[1], second);
}

TEST(StreamInitExclusive, NotAlignedRebuildsThenRetriesEqualDuration) {
    FakeAudioClientInit fake;
    AudioFormat want{48000, 2, 16, false};
    fake.supportedFormats.push_back(want);
    fake.failFirstInitWithNotAligned = true;
    fake.minPeriod = 300000;     // 30 ms; first Initialize would use this
    fake.alignedFrames = 480;    // 480 frames @ 48 kHz = 10 ms = 100000
    StreamInitRequest req;
    req.requested = &want;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.rebuildCount, 1);
    EXPECT_EQ(fake.initializeCount, 2);
    EXPECT_EQ(fake.lastDuration, 100000);
    EXPECT_EQ(fake.lastPeriodicity, 100000);
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_EXCLUSIVE);
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK));
    EXPECT_EQ(out.actualFormat, want);
}

TEST(StreamInitExclusive, NotAlignedRebuildFailurePropagates) {
    FakeAudioClientInit fake;
    AudioFormat want{48000, 2, 16, false};
    fake.supportedFormats.push_back(want);
    fake.failFirstInitWithNotAligned = true;
    fake.rebuildHr = E_FAIL;
    StreamInitRequest req;
    req.requested = &want;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(fake.rebuildCount, 1);
    EXPECT_EQ(fake.initializeCount, 1);
}

TEST(StreamInitExclusive, CaptureNoneSupportedFailsWithoutInitialize) {
    FakeAudioClientInit fake;
    StreamInitRequest req;
    req.direction = StreamInitDirection::Capture;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.code, static_cast<long>(AUDCLNT_E_UNSUPPORTED_FORMAT));
    EXPECT_EQ(fake.initializeCount, 0);
}

TEST(StreamInitExclusive, AutoConvertForceFailsWithoutInitialize) {
    FakeAudioClientInit fake;
    AudioFormat want{48000, 2, 16, false};
    fake.supportedFormats.push_back(want);
    StreamInitRequest req;
    req.requested = &want;
    req.params.autoConvert = AutoConvert::Force;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(fake.initializeCount, 0);
    EXPECT_EQ(fake.rebuildCount, 0);
    EXPECT_TRUE(fake.probedFormats.empty());
}

TEST(StreamInitExclusive, AutoConvertOffKeepsProbeAndEventCallback) {
    FakeAudioClientInit fake;
    AudioFormat want{48000, 2, 16, false};
    fake.supportedFormats.push_back(want);
    StreamInitRequest req;
    req.requested = &want;
    req.params.autoConvert = AutoConvert::Off;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_EXCLUSIVE);
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK));
    EXPECT_EQ(out.actualFormat, want);
    EXPECT_EQ(fake.initializeCount, 1);
}

TEST(StreamInitExclusive, AutoConvertOffNotAlignedRebuildsThenRetries) {
    FakeAudioClientInit fake;
    AudioFormat want{48000, 2, 16, false};
    fake.supportedFormats.push_back(want);
    fake.failFirstInitWithNotAligned = true;
    fake.minPeriod = 300000;
    fake.alignedFrames = 480;
    StreamInitRequest req;
    req.requested = &want;
    req.params.autoConvert = AutoConvert::Off;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.rebuildCount, 1);
    EXPECT_EQ(fake.initializeCount, 2);
    EXPECT_EQ(fake.lastDuration, 100000);
    EXPECT_EQ(fake.lastPeriodicity, 100000);
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK));
    EXPECT_EQ(out.actualFormat, want);
}

TEST(StreamInitExclusive, BufferMsSetsEqualDurationAndPeriodicity) {
    FakeAudioClientInit fake;
    AudioFormat want{48000, 2, 16, false};
    fake.supportedFormats.push_back(want);
    StreamInitRequest req;
    req.requested = &want;
    req.params.bufferMs = 50;
    StreamInitOutcome out;
    Result r = streamInitExclusive(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastDuration, 500000);
    EXPECT_EQ(fake.lastPeriodicity, 500000);
}

TEST(StreamInitExclusive, DispatcherRoutesExclusiveShareMode) {
    FakeAudioClientInit fake;
    AudioFormat want{48000, 2, 16, false};
    fake.supportedFormats.push_back(want);
    StreamInitRequest req;
    req.requested = &want;
    StreamInitOutcome out;
    Result r = streamInit(AUDCLNT_SHAREMODE_EXCLUSIVE, fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_EXCLUSIVE);
}
