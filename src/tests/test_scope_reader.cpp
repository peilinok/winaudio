#include <gtest/gtest.h>
#include <vector>
#include "FakeScopeReader.h"

using namespace wa;

TEST(FakeScopeReader, WrittenAndLatestMono) {
    FakeScopeReader r(1);
    const float s[] = {1.f, 2.f, 3.f, 4.f, 5.f};
    r.pushMono(s, 5);
    EXPECT_EQ(r.written(), 5u);
    EXPECT_EQ(r.channels(), 1u);

    float out[3] = {};
    uint64_t end = 0;
    ASSERT_TRUE(r.snapshotLatest(3, out, end));
    EXPECT_EQ(end, 5u);
    EXPECT_FLOAT_EQ(out[0], 3.f);
    EXPECT_FLOAT_EQ(out[1], 4.f);
    EXPECT_FLOAT_EQ(out[2], 5.f);
}

TEST(FakeScopeReader, EndingAtMonoAndChannel) {
    FakeScopeReader r(2);
    // frames: (0,10), (1,11), (2,12), (3,13)
    const float in[] = {0, 10, 1, 11, 2, 12, 3, 13};
    r.pushInterleaved(in, 4);

    float mono[2] = {};
    ASSERT_TRUE(r.snapshotEndingAt(3, 2, mono)); // frames 1,2 -> avg (1+11)/2, (2+12)/2
    EXPECT_FLOAT_EQ(mono[0], 6.f);
    EXPECT_FLOAT_EQ(mono[1], 7.f);

    float ch1[2] = {};
    ASSERT_TRUE(r.snapshotChannelEndingAt(1, 3, 2, ch1));
    EXPECT_FLOAT_EQ(ch1[0], 11.f);
    EXPECT_FLOAT_EQ(ch1[1], 12.f);

    EXPECT_FALSE(r.snapshotEndingAt(5, 2, mono));           // end beyond written
    EXPECT_FALSE(r.snapshotChannelEndingAt(2, 3, 1, ch1));  // bad channel
}

TEST(FakeScopeReader, LatestEndIdxMatchesEndingAt) {
    FakeScopeReader r(1);
    std::vector<float> s(16);
    for (size_t i = 0; i < s.size(); ++i) s[i] = (float)i;
    r.pushMono(s.data(), s.size());

    float a[4] = {}, b[4] = {};
    uint64_t end = 0;
    ASSERT_TRUE(r.snapshotLatest(4, a, end));
    ASSERT_TRUE(r.snapshotEndingAt(end, 4, b));
    for (int i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(a[i], b[i]);
}

TEST(FakeScopeReader, EmptyReaderFailsSnapshots) {
    FakeScopeReader r(1);
    float out[4] = {};
    uint64_t end = 0;
    EXPECT_EQ(r.written(), 0u);
    EXPECT_FALSE(r.snapshotLatest(4, out, end));
    EXPECT_FALSE(r.snapshotEndingAt(4, 4, out));
    EXPECT_FALSE(r.snapshotChannelEndingAt(0, 4, 4, out));
}
