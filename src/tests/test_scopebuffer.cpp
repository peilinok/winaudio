#include <gtest/gtest.h>
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
