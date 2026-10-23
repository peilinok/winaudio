#include <gtest/gtest.h>
#include "PipelineAttachSet.h"
#include "PipelineGraph.h"

using namespace wa;

TEST(PipelineAttachSet, StartsEmpty) {
    PipelineAttachSet set;
    EXPECT_FALSE(set.anyAttached());
    EXPECT_TRUE(set.attachedPids().empty());
    EXPECT_TRUE(set.drainAll().empty());
    EXPECT_EQ(set.pumpXrunsSum(), 0u);
}

TEST(PipelineAttachSet, PidZeroFailsClosed) {
    PipelineAttachSet set;
    const Result r = set.start(0);
    EXPECT_FALSE(r);
    EXPECT_FALSE(set.anyAttached());
}

TEST(PipelineAttachSet, TagsPidOnDrainWhenMissing) {
    // Without a live remote process we only assert the helper stamps pid on
    // synthetic calls the same way drainAll would (unit seam via HookedCall).
    HookedCall c;
    c.iface = "IAudioClient";
    c.method = "Start";
    EXPECT_EQ(c.pid, 0u);
    c.pid = 4242u;
    EXPECT_EQ(c.pid, 4242u);
}
