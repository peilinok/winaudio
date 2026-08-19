#include <gtest/gtest.h>
#include "StreamInit.h"
#include "AudioFormat.h"
#include <objbase.h>

using namespace wa;

namespace {

constexpr REFERENCE_TIME kSharedDefaultDuration = 1'000'000; // 100 ms in 100-ns units

class FakeAudioClientInit : public AudioClientInit {
public:
    AudioFormat mixFormat{48000, 2, 16, false};
    HRESULT mixHr = S_OK;
    HRESULT initHr = S_OK;

    AUDCLNT_SHAREMODE lastShareMode = AUDCLNT_SHAREMODE_SHARED;
    DWORD lastFlags = 0;
    REFERENCE_TIME lastDuration = 0;
    REFERENCE_TIME lastPeriodicity = -1;
    AudioFormat lastFormat{};
    int initializeCount = 0;

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
        return initHr;
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

TEST(StreamInitShared, RequestedDefaultAddsAutoconvertAndSrc) {
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

TEST(StreamInitShared, RequestedPlusLoopbackExtraORsAutoconvert) {
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
