#include <gtest/gtest.h>
#include <string>
#include "IAudioBackend.h"
#include "RingBuffer.h"
#include "WasapiStream.h"

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
