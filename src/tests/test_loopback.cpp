#include <gtest/gtest.h>
#include <string>
#include "ApplicationLoopbackCapture.h"
#include "AudioFormat.h"
#include "IAudioBackend.h"
#include "RingBuffer.h"
#include "StreamInit.h"
#include "WasapiStream.h"
#include <objbase.h>
#include <windows.h>

using namespace wa;

TEST(CaptureSource, DefaultsToEndpoint) {
    CaptureSource source;
    EXPECT_EQ(source.kind, CaptureSourceKind::Endpoint);
    EXPECT_TRUE(source.deviceId.empty());
}

TEST(CaptureSource, CanRepresentSystemLoopbackRenderDevice) {
    CaptureSource source{CaptureSourceKind::SystemLoopback, L"render-device-id"};
    EXPECT_EQ(source.kind, CaptureSourceKind::SystemLoopback);
    EXPECT_EQ(source.deviceId, L"render-device-id");
}

TEST(CaptureSource, CanRepresentApplicationLoopbackProcess) {
    CaptureSource source{CaptureSourceKind::ApplicationLoopback, L"", 4242u};
    EXPECT_EQ(source.kind, CaptureSourceKind::ApplicationLoopback);
    EXPECT_TRUE(source.deviceId.empty());
    EXPECT_EQ(source.processId, 4242u);
    EXPECT_EQ(source.processLoopbackMode, ProcessLoopbackMode::IncludeTree);
}

TEST(CaptureSource, DefaultsProcessLoopbackModeToIncludeTree) {
    CaptureSource source;
    EXPECT_EQ(source.processLoopbackMode, ProcessLoopbackMode::IncludeTree);
}

TEST(CaptureSource, CanRepresentApplicationLoopbackExcludeTree) {
    CaptureSource source{CaptureSourceKind::ApplicationLoopback, L"", 4242u,
                         ProcessLoopbackMode::ExcludeTree};
    EXPECT_EQ(source.kind, CaptureSourceKind::ApplicationLoopback);
    EXPECT_EQ(source.processId, 4242u);
    EXPECT_EQ(source.processLoopbackMode, ProcessLoopbackMode::ExcludeTree);
}

TEST(WasapiSystemLoopbackCaptureStream, RejectsExclusiveInOpen) {
    RingBuffer ring(4096);
    WasapiSystemLoopbackCaptureStream stream(WasapiMode::Exclusive, nullptr);

    Result r = stream.open(L"", AudioFormat{}, &ring, StreamParams{});

    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_NE(r.message.find("loopback"), std::string::npos);
    EXPECT_NE(r.message.find("Shared"), std::string::npos);
}

TEST(WasapiSystemLoopbackCaptureStream, IdleTimeoutSilenceKeepsWallClockCadence) {
    EXPECT_EQ(loopbackSilenceFramesForTimeout(48000, 200), 9600u);
    EXPECT_EQ(loopbackSilenceFramesForTimeout(44100, 50), 2205u);
    EXPECT_EQ(loopbackSilenceFramesForTimeout(0, 200), 0u);
}

TEST(WasapiSystemLoopbackCaptureStream, IdleSilenceWhenWakeHasNoPackets) {
    EXPECT_TRUE(shouldWriteLoopbackIdleSilence(WAIT_TIMEOUT, S_OK, false, false));
    EXPECT_TRUE(shouldWriteLoopbackIdleSilence(WAIT_OBJECT_0, S_OK, false, false));
    EXPECT_FALSE(shouldWriteLoopbackIdleSilence(WAIT_TIMEOUT, S_OK, true, false));
    EXPECT_FALSE(shouldWriteLoopbackIdleSilence(WAIT_TIMEOUT, S_OK, false, true));
    EXPECT_FALSE(shouldWriteLoopbackIdleSilence(WAIT_TIMEOUT, AUDCLNT_E_DEVICE_INVALIDATED,
                                               false, false));
}

TEST(WasapiSystemLoopbackCaptureStream, IdleSilenceUsesElapsedTimeWithTimeoutCap) {
    EXPECT_EQ(loopbackSilenceFramesForElapsed(48000, 10, 200), 480u);
    EXPECT_EQ(loopbackSilenceFramesForElapsed(48000, 250, 200), 9600u);
    EXPECT_EQ(loopbackSilenceFramesForElapsed(48000, 0, 200), 0u);
    EXPECT_EQ(loopbackSilenceFramesForElapsed(0, 10, 200), 0u);
}

TEST(WasapiSystemLoopbackCaptureStream, SilentPacketFramesComeFromWasapiFlag) {
    EXPECT_EQ(captureSilentPacketFrames(480, AUDCLNT_BUFFERFLAGS_SILENT), 480u);
    EXPECT_EQ(captureSilentPacketFrames(480, 0), 0u);
}

TEST(WasapiSilentRenderStream, RejectsExclusiveInOpen) {
    WasapiSilentRenderStream stream(WasapiMode::Exclusive, nullptr);

    Result r = stream.open(L"", AudioFormat{}, nullptr, StreamParams{});

    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_NE(r.message.find("silent render"), std::string::npos);
    EXPECT_NE(r.message.find("Shared"), std::string::npos);
}

TEST(ApplicationLoopbackCaptureStream, RejectsExclusiveInOpen) {
    RingBuffer ring(4096);
    ApplicationLoopbackCaptureStream stream(WasapiMode::Exclusive, 4242u, nullptr);

    Result r = stream.open(L"", AudioFormat{}, &ring, StreamParams{});

    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_NE(r.message.find("application loopback"), std::string::npos);
    EXPECT_NE(r.message.find("Shared"), std::string::npos);
}

TEST(ApplicationLoopbackCaptureStream, RejectsZeroPid) {
    RingBuffer ring(4096);
    ApplicationLoopbackCaptureStream stream(WasapiMode::Shared, 0u, nullptr);

    Result r = stream.open(L"", AudioFormat{}, &ring, StreamParams{});

    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_NE(r.message.find("PID"), std::string::npos);
}

TEST(ApplicationLoopbackCaptureStream, AcceptsSharedOpenWithValidPid) {
    RingBuffer ring(4096);
    ApplicationLoopbackCaptureStream stream(WasapiMode::Shared, 4242u, nullptr);

    Result r = stream.open(L"", AudioFormat{}, &ring, StreamParams{});

    EXPECT_TRUE(static_cast<bool>(r));
}

TEST(ApplicationLoopbackCaptureStream, DefaultFormatMatchesProcessLoopbackSampleFallback) {
    AudioFormat fmt = defaultApplicationLoopbackFormat();

    EXPECT_EQ(fmt.sampleRate, 44100u);
    EXPECT_EQ(fmt.channels, 2u);
    EXPECT_EQ(fmt.bitsPerSample, 16u);
    EXPECT_FALSE(fmt.isFloat);
}

namespace {

constexpr REFERENCE_TIME kSharedDefaultDuration = 1'000'000;

class FakeAppLoopbackClient : public AudioClientInit {
public:
    AudioFormat mixFormat{48000, 2, 16, false};
    HRESULT mixHr = S_OK;
    HRESULT initHr = S_OK;
    bool failFirstInit = false;

    AUDCLNT_SHAREMODE lastShareMode = AUDCLNT_SHAREMODE_SHARED;
    DWORD lastFlags = 0;
    REFERENCE_TIME lastDuration = 0;
    REFERENCE_TIME lastPeriodicity = -1;
    AudioFormat lastFormat{};
    int initializeCount = 0;
    int setClientPropertiesCount = 0;
    int isOffloadCapableCount = 0;
    int initializeCountAtSetClientProperties = -1;
    AudioClientProperties lastProps{};
    HRESULT propsHr = S_OK;
    HRESULT offloadHr = S_OK;
    BOOL offloadCapable = TRUE;

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
        if (failFirstInit && initializeCount == 1) return E_FAIL;
        return initHr;
    }

    HRESULT isFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*) override {
        return E_NOTIMPL;
    }
    HRESULT getDevicePeriod(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT getBufferSize(UINT32*) override { return E_NOTIMPL; }
    HRESULT rebuild() override { return E_NOTIMPL; }
    HRESULT setClientProperties(const AudioClientProperties& props) override {
        ++setClientPropertiesCount;
        initializeCountAtSetClientProperties = initializeCount;
        lastProps = props;
        return propsHr;
    }
    HRESULT isOffloadCapable(AUDIO_STREAM_CATEGORY, BOOL* capable) override {
        ++isOffloadCapableCount;
        if (capable) *capable = offloadCapable;
        return offloadHr;
    }
};

} // namespace

TEST(ApplicationLoopbackStreamInit, MixDefaultUsesEventCallbackAndLoopback) {
    FakeAppLoopbackClient fake;
    StreamInitRequest req;
    StreamInitOutcome out;
    Result r = streamInitApplicationLoopback(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastShareMode, AUDCLNT_SHAREMODE_SHARED);
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                 AUDCLNT_STREAMFLAGS_LOOPBACK));
    EXPECT_EQ(fake.lastDuration, kSharedDefaultDuration);
    EXPECT_EQ(fake.lastPeriodicity, 0);
    EXPECT_EQ(out.actualFormat, fake.mixFormat);
    EXPECT_EQ(out.frameBytes, fake.mixFormat.blockAlign());
    EXPECT_EQ(fake.initializeCount, 1);
}

TEST(ApplicationLoopbackStreamInit, MixInitFailureRetriesWith44100Requested) {
    FakeAppLoopbackClient fake;
    fake.failFirstInit = true;
    StreamInitRequest req;
    StreamInitOutcome out;
    Result r = streamInitApplicationLoopback(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    const AudioFormat fallback = defaultApplicationLoopbackFormat();
    EXPECT_EQ(fake.initializeCount, 2);
    EXPECT_EQ(fake.lastFormat, fallback);
    EXPECT_EQ(out.actualFormat, fallback);
    const DWORD expected = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                           AUDCLNT_STREAMFLAGS_LOOPBACK |
                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    EXPECT_EQ(fake.lastFlags, expected);
}

TEST(ApplicationLoopbackStreamInit, MixGetMixFormatFailureRetriesWith44100Requested) {
    FakeAppLoopbackClient fake;
    fake.mixHr = E_FAIL;
    StreamInitRequest req;
    StreamInitOutcome out;
    Result r = streamInitApplicationLoopback(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    const AudioFormat fallback = defaultApplicationLoopbackFormat();
    EXPECT_EQ(fake.initializeCount, 1);
    EXPECT_EQ(fake.lastFormat, fallback);
    EXPECT_EQ(out.actualFormat, fallback);
    const DWORD expected = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                           AUDCLNT_STREAMFLAGS_LOOPBACK |
                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    EXPECT_EQ(fake.lastFlags, expected);
}

TEST(ApplicationLoopbackStreamInit, RequestedInitFailureDoesNotFallback) {
    FakeAppLoopbackClient fake;
    fake.initHr = E_FAIL;
    AudioFormat want{48000, 1, 16, false};
    StreamInitRequest req;
    req.requested = &want;
    StreamInitOutcome out;
    Result r = streamInitApplicationLoopback(fake, req, out);
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(fake.initializeCount, 1);
    EXPECT_EQ(fake.lastFormat, want);
}

TEST(ApplicationLoopbackStreamInit, RequestedOffOmitsAutoConvertKeepsLoopback) {
    FakeAppLoopbackClient fake;
    AudioFormat want{48000, 2, 24, false};
    StreamInitRequest req;
    req.requested = &want;
    req.params.autoConvert = AutoConvert::Off;
    StreamInitOutcome out;
    Result r = streamInitApplicationLoopback(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                 AUDCLNT_STREAMFLAGS_LOOPBACK));
    EXPECT_EQ(out.actualFormat, want);
}

TEST(ApplicationLoopbackStreamInit, BufferMsSetsDuration) {
    FakeAppLoopbackClient fake;
    StreamInitRequest req;
    req.params.bufferMs = 50;
    StreamInitOutcome out;
    Result r = streamInitApplicationLoopback(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.lastDuration, static_cast<REFERENCE_TIME>(50) * 10'000);
}

TEST(ApplicationLoopbackStreamInit, MixForceAddsAutoConvertAndLoopback) {
    FakeAppLoopbackClient fake;
    StreamInitRequest req;
    req.params.autoConvert = AutoConvert::Force;
    StreamInitOutcome out;
    Result r = streamInitApplicationLoopback(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    const DWORD expected = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                           AUDCLNT_STREAMFLAGS_LOOPBACK |
                           AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    EXPECT_EQ(fake.lastFlags, expected);
    EXPECT_EQ(fake.initializeCount, 1);
}

TEST(ApplicationLoopbackClientProperties, DisabledSkipsSetClientProperties) {
    FakeAppLoopbackClient fake;
    StreamInitRequest req;
    StreamInitOutcome out;
    Result r = openApplicationLoopbackClient(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.setClientPropertiesCount, 0);
    EXPECT_EQ(fake.isOffloadCapableCount, 0);
    EXPECT_EQ(fake.initializeCount, 1);
}

TEST(ApplicationLoopbackClientProperties, EnabledSetsMappedFieldsBeforeInitialize) {
    FakeAppLoopbackClient fake;
    StreamInitRequest req;
    req.params.clientProperties.enabled = true;
    req.params.clientProperties.category = AudioCategory::Media;
    req.params.clientProperties.offload = false;
    req.params.clientProperties.option = StreamOption::Raw;
    StreamInitOutcome out;
    Result r = openApplicationLoopbackClient(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.setClientPropertiesCount, 1);
    EXPECT_EQ(fake.isOffloadCapableCount, 0);
    EXPECT_EQ(fake.initializeCountAtSetClientProperties, 0);
    EXPECT_EQ(fake.initializeCount, 1);
    EXPECT_EQ(fake.lastProps.cbSize, sizeof(AudioClientProperties));
    EXPECT_EQ(fake.lastProps.eCategory, AudioCategory_Media);
    EXPECT_EQ(fake.lastProps.bIsOffload, FALSE);
    EXPECT_EQ(fake.lastProps.Options, AUDCLNT_STREAMOPTIONS_RAW);
}

TEST(ApplicationLoopbackClientProperties, OffloadChecksCapability) {
    FakeAppLoopbackClient fake;
    StreamInitRequest req;
    req.params.clientProperties.enabled = true;
    req.params.clientProperties.offload = true;
    req.params.clientProperties.category = AudioCategory::GameMedia;
    StreamInitOutcome out;
    Result r = openApplicationLoopbackClient(fake, req, out);
    ASSERT_TRUE(static_cast<bool>(r)) << r.message;
    EXPECT_EQ(fake.isOffloadCapableCount, 1);
    EXPECT_EQ(fake.setClientPropertiesCount, 1);
    EXPECT_EQ(fake.lastProps.bIsOffload, TRUE);
    EXPECT_EQ(fake.lastProps.eCategory, AudioCategory_GameMedia);
}

TEST(ApplicationLoopbackClientProperties, SetClientPropertiesFailureFailsOpen) {
    FakeAppLoopbackClient fake;
    fake.propsHr = E_FAIL;
    StreamInitRequest req;
    req.params.clientProperties.enabled = true;
    StreamInitOutcome out;
    Result r = openApplicationLoopbackClient(fake, req, out);
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(fake.setClientPropertiesCount, 1);
    EXPECT_EQ(fake.initializeCount, 0);
}

TEST(ApplicationLoopbackClientProperties, OffloadNotCapableFailsOpen) {
    FakeAppLoopbackClient fake;
    fake.offloadCapable = FALSE;
    StreamInitRequest req;
    req.params.clientProperties.enabled = true;
    req.params.clientProperties.offload = true;
    StreamInitOutcome out;
    Result r = openApplicationLoopbackClient(fake, req, out);
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(fake.isOffloadCapableCount, 1);
    EXPECT_EQ(fake.setClientPropertiesCount, 0);
    EXPECT_EQ(fake.initializeCount, 0);
}
