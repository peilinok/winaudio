#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include "ScopeBuffer.h"
using namespace wa;
TEST(ScopeBuffer, NotEnoughYet) {
    ScopeBuffer sb(1024); float out[64]; uint64_t end;
    EXPECT_FALSE(sb.snapshotLatest(64, out, end));     // nothing written
}
TEST(ScopeBuffer, ReturnsRecentWindow) {
    ScopeBuffer sb(1024);
    std::vector<float> in(256); for (size_t i=0;i<256;++i) in[i]=(float)i;
    sb.push(in.data(), 256);
    EXPECT_EQ(sb.totalWritten(), 256u);
    float out[64]; uint64_t end;
    ASSERT_TRUE(sb.snapshotLatest(64, out, end));
    EXPECT_EQ(end, 256u);
    EXPECT_FLOAT_EQ(out[0], 192.0f);                    // oldest of last 64 = sample 192
    EXPECT_FLOAT_EQ(out[63], 255.0f);
}
TEST(ScopeBuffer, RejectsWindowLargerThanHalfCapacity) {
    ScopeBuffer sb(100); float out[80]; uint64_t end;
    // push enough, but n>cap/2 is disallowed by contract -> returns false
    std::vector<float> in(100,1.0f); sb.push(in.data(),100);
    EXPECT_FALSE(sb.snapshotLatest(80, out, end));
}
TEST(ScopeBuffer, SnapshotsInterleavedChannelsAndMonoDownmix) {
    ScopeBuffer sb(16, 2);
    const float in[] = {
        1.0f, 10.0f,
        2.0f, 20.0f,
        3.0f, 30.0f,
        4.0f, 40.0f,
    };
    sb.pushInterleaved(in, 4);

    float out[2] = {};
    uint64_t end = 0;
    ASSERT_TRUE(sb.snapshotLatestChannel(0, 2, out, end));
    EXPECT_EQ(end, 4u);
    EXPECT_FLOAT_EQ(out[0], 3.0f);
    EXPECT_FLOAT_EQ(out[1], 4.0f);

    ASSERT_TRUE(sb.snapshotLatestChannel(1, 2, out, end));
    EXPECT_EQ(end, 4u);
    EXPECT_FLOAT_EQ(out[0], 30.0f);
    EXPECT_FLOAT_EQ(out[1], 40.0f);

    ASSERT_TRUE(sb.snapshotLatest(2, out, end));
    EXPECT_EQ(end, 4u);
    EXPECT_FLOAT_EQ(out[0], 16.5f);
    EXPECT_FLOAT_EQ(out[1], 22.0f);

    EXPECT_FALSE(sb.snapshotLatestChannel(2, 2, out, end));
}
TEST(ScopeBuffer, SnapshotChannelEndingAtUsesRequestedWindow) {
    ScopeBuffer sb(16, 2);
    std::vector<float> in;
    for (int i = 0; i < 10; ++i) {
        in.push_back((float)i);
        in.push_back((float)(100 + i));
    }
    sb.pushInterleaved(in.data(), 10);

    float out[3] = {};
    ASSERT_TRUE(sb.snapshotChannelEndingAt(1, 6, 3, out));
    EXPECT_FLOAT_EQ(out[0], 103.0f);
    EXPECT_FLOAT_EQ(out[1], 104.0f);
    EXPECT_FLOAT_EQ(out[2], 105.0f);

    EXPECT_FALSE(sb.snapshotChannelEndingAt(1, 11, 3, out));

    std::vector<float> more;
    for (int i = 10; i < 26; ++i) {
        more.push_back((float)i);
        more.push_back((float)(100 + i));
    }
    sb.pushInterleaved(more.data(), 16);
    EXPECT_FALSE(sb.snapshotChannelEndingAt(1, 6, 3, out));
}
TEST(ScopeBuffer, ConcurrentSpscConsistency) {
    ScopeBuffer sb(8192);
    std::atomic<bool> stop{false};
    std::thread prod([&]{ float s=0; std::vector<float> buf(128);
        while(!stop){ for(auto&v:buf) v=s++; sb.push(buf.data(),128);} });
    for (int i=0;i<2000;++i){ float out[1024]; uint64_t end;
        if (sb.snapshotLatest(1024,out,end)) {          // window must be contiguous/monotonic
            for (size_t k=1;k<1024;++k) EXPECT_FLOAT_EQ(out[k], out[k-1]+1.0f); } }
    stop=true; prod.join();
}
