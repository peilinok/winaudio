#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <numeric>
#include "RingBuffer.h"

using wa::RingBuffer;

TEST(RingBuffer, WriteThenReadRoundTrip) {
    RingBuffer rb(16);
    uint8_t in[4] = {1, 2, 3, 4};
    EXPECT_EQ(rb.write(in, 4), 4u);
    EXPECT_EQ(rb.availableRead(), 4u);
    uint8_t out[4] = {};
    EXPECT_EQ(rb.read(out, 4), 4u);
    EXPECT_EQ(0, memcmp(in, out, 4));
    EXPECT_EQ(rb.availableRead(), 0u);
}

TEST(RingBuffer, OverrunCountedWhenFull) {
    RingBuffer rb(4);
    uint8_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ(rb.write(in, 8), 4u);     // only 4 fit
    EXPECT_EQ(rb.overruns(), 1u);
}

TEST(RingBuffer, UnderrunCountedWhenEmpty) {
    RingBuffer rb(4);
    uint8_t out[4] = {};
    EXPECT_EQ(rb.read(out, 4), 0u);
    EXPECT_EQ(rb.underruns(), 1u);
}

TEST(RingBuffer, WrapAround) {
    RingBuffer rb(8);
    uint8_t a[6] = {1,2,3,4,5,6};
    uint8_t tmp[6] = {};
    rb.write(a, 6);
    rb.read(tmp, 6);            // advance past the middle
    uint8_t b[6] = {7,8,9,10,11,12};
    EXPECT_EQ(rb.write(b, 6), 6u);  // must wrap
    uint8_t out[6] = {};
    EXPECT_EQ(rb.read(out, 6), 6u);
    EXPECT_EQ(0, memcmp(b, out, 6));
}

TEST(RingBuffer, SpscStress) {
    const size_t N = 1'000'000;
    RingBuffer rb(1024);
    std::thread producer([&] {
        size_t written = 0;
        while (written < N) {
            uint8_t byte = static_cast<uint8_t>(written & 0xFF);
            if (rb.write(&byte, 1) == 1) ++written;
        }
    });
    size_t read = 0;
    while (read < N) {
        uint8_t byte;
        if (rb.read(&byte, 1) == 1) {
            ASSERT_EQ(byte, static_cast<uint8_t>(read & 0xFF));
            ++read;
        }
    }
    producer.join();
    EXPECT_EQ(read, N);
}
