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
