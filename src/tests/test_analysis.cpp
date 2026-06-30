#include <gtest/gtest.h>
#include <vector>
#include "Analysis.h"
using namespace wa;
TEST(Analysis, NothingBeforeFirstWindow) {
    uint64_t next = 0; std::vector<uint64_t> got;
    size_t n = advanceAnalysis(2047, next, 2048, 512, 8, [&](uint64_t e){ got.push_back(e); });
    EXPECT_EQ(n, 0u); EXPECT_TRUE(got.empty());
}
TEST(Analysis, EmitsHopBoundaries) {
    uint64_t next = 0; std::vector<uint64_t> got;
    // written = 2048 + 3*512 = 3584 -> first window at 2048, then 2560, 3072, 3584
    size_t n = advanceAnalysis(3584, next, 2048, 512, 8, [&](uint64_t e){ got.push_back(e); });
    EXPECT_EQ(n, 4u);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0], 2048u); EXPECT_EQ(got[1], 2560u); EXPECT_EQ(got[2], 3072u); EXPECT_EQ(got[3], 3584u);
    EXPECT_EQ(next, 3584u);     // last emitted boundary
}
TEST(Analysis, FastForwardSkipsStaleBacklog) {
    uint64_t next = 0;
    // jump way ahead: written huge -> should process only maxCatchup (8) most-recent hops, not thousands
    size_t n = advanceAnalysis(2048 + 100000ull*512, next, 2048, 512, 8, [&](uint64_t){});
    EXPECT_LE(n, 8u);          // capped at maxCatchup
    EXPECT_GT(n, 0u);
}
TEST(Analysis, ResumesFromPersistedIndex) {
    uint64_t next = 2048; std::vector<uint64_t> got;
    size_t n = advanceAnalysis(2048 + 512, next, 2048, 512, 8, [&](uint64_t e){ got.push_back(e); });
    EXPECT_EQ(n, 1u); EXPECT_EQ(got[0], 2560u); EXPECT_EQ(next, 2560u);
}
TEST(Analysis, ChainedCallsEmitEveryHopNoGap) {
    uint64_t next = 0;
    std::vector<uint64_t> got;
    auto cb = [&](uint64_t e){ got.push_back(e); };
    uint64_t written = 2048;                 // first full window
    for (int i = 0; i < 6; ++i) {            // simulate per-frame calls; written grows one hop each frame
        advanceAnalysis(written, next, 2048, 512, 8, cb);
        written += 512;
    }
    // contiguous hop boundaries with NO gaps: 2048,2560,3072,3584,4096,4608
    ASSERT_GE(got.size(), 5u);
    EXPECT_EQ(got[0], 2048u);
    for (size_t k = 1; k < got.size(); ++k) EXPECT_EQ(got[k], got[k-1] + 512u);
}
