#include <gtest/gtest.h>
#include <string>
#include "IAudioBackend.h"
#include "RingBuffer.h"
#include "WasapiStream.h"
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

TEST(WasapiSystemLoopbackCaptureStream, IdleSilenceOnlyWhenTimeoutHasNoPackets) {
    EXPECT_TRUE(shouldWriteLoopbackIdleSilence(WAIT_TIMEOUT, S_OK, false, false));
    EXPECT_FALSE(shouldWriteLoopbackIdleSilence(WAIT_OBJECT_0, S_OK, false, false));
    EXPECT_FALSE(shouldWriteLoopbackIdleSilence(WAIT_TIMEOUT, S_OK, true, false));
    EXPECT_FALSE(shouldWriteLoopbackIdleSilence(WAIT_TIMEOUT, S_OK, false, true));
    EXPECT_FALSE(shouldWriteLoopbackIdleSilence(WAIT_TIMEOUT, AUDCLNT_E_DEVICE_INVALIDATED,
                                               false, false));
}
