#include <gtest/gtest.h>
#include "StreamParams.h"
using namespace wa;

TEST(StreamParams, DefaultIsAllDefault) {
    StreamParams p;
    EXPECT_TRUE(p.isDefault());
    EXPECT_FALSE(p.anyClientProps());
    EXPECT_EQ(p.bufferMs, 0u);
}

TEST(StreamParams, Predicates) {
    StreamParams a; a.category = AudioCategory::Communications;
    EXPECT_TRUE(a.anyClientProps()); EXPECT_FALSE(a.isDefault());
    StreamParams b; b.option = StreamOption::Raw;
    EXPECT_TRUE(b.anyClientProps()); EXPECT_FALSE(b.isDefault());
    StreamParams c; c.offload = OffloadMode::Force;
    EXPECT_TRUE(c.anyClientProps()); EXPECT_FALSE(c.isDefault());
    StreamParams d; d.ducking = DuckingMode::OptOut;      // ducking 不属于 client-props
    EXPECT_FALSE(d.anyClientProps()); EXPECT_FALSE(d.isDefault());
    StreamParams e; e.bufferMs = 50;                       // bufferMs 也不属于
    EXPECT_FALSE(e.anyClientProps()); EXPECT_FALSE(e.isDefault());
}
