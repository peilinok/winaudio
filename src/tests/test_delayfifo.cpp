#include <gtest/gtest.h>
#include <vector>
#include "DelayFifo.h"
using namespace wa;

TEST(DelayFifo, BasicPushPop) {
    DelayFifo f(1, /*target*/480, /*cap*/4800, /*deadband*/96);
    std::vector<float> in(480, 0.5f), out(480, 0.f);
    f.pushFrames(in.data(), 480);
    EXPECT_EQ(f.fillFrames(), 480u);
    size_t got = f.popFrames(out.data(), 480);
    EXPECT_GT(got, 0u);
}

TEST(DelayFifo, AbsorbsSustainedDriftWithoutRunaway) {
    DelayFifo f(1, 480, 4800, 96);
    std::vector<float> in(101), out(200);
    for (int r = 0; r < 5000; ++r) {
        for (auto& v : in) v = (float)r;
        f.pushFrames(in.data(), 101);     // producer faster
        f.popFrames(out.data(), 100);     // consumer slower -> +1 frame/round drift
    }
    // The controller must absorb the +1/round drift: occupancy stays bounded (no runaway to capacity 4800)
    EXPECT_LT(f.fillFrames(), 480u + 6u * 96u);   // well under capacity
    EXPECT_GT(f.driftFixes(), 0u);                // controller acted (dropped frames)
}

TEST(DelayFifo, DuplicatesWhenStarved) {
    DelayFifo f(1, 480, 4800, 96);
    std::vector<float> in(99), out(200);
    // prime above target, then run consumer faster -> occupancy falls -> controller duplicates
    std::vector<float> prime(600, 0.f); f.pushFrames(prime.data(), 600);
    uint64_t before = f.driftFixes();
    for (int r = 0; r < 3000; ++r) { for(auto&v:in)v=(float)r; f.pushFrames(in.data(),99); f.popFrames(out.data(),100); }
    EXPECT_GT(f.driftFixes(), before);            // dup corrections happened
    EXPECT_GT(f.fillFrames(), 0u);                // didn't fully underrun
}
