#include <gtest/gtest.h>
#include "PumpWindow.h"
#include <memory>

using namespace wa;

TEST(PumpWindow, ExcludesOldSlotsAndObservesCompletionWithoutCounterChange) {
    auto ring = std::make_unique<hook_ipc::Ring>();
    ring->pumpEnabled = 1;
    ring->pumpWriteIndex = 1;
    strcpy_s(ring->pumpSlots[0].method, "GetBuffer");
    ring->pumpSlots[0].timeMs = 10;
    PumpWindow window;
    window.begin(*ring);
    EXPECT_TRUE(window.poll(*ring).empty());
    // The real producer reserves its index before publishing slot contents.
    ring->pumpWriteIndex = 2;
    EXPECT_TRUE(window.poll(*ring).empty());
    strcpy_s(ring->pumpSlots[1].method, "ReleaseBuffer");
    ring->pumpSlots[1].timeMs = 20;
    const auto first = window.poll(*ring);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first.front().call.method, "ReleaseBuffer");
    const auto identity = first.front().sequence;
    EXPECT_EQ(window.poll(*ring).front().sequence, identity);
    ring->pumpSlots[1].timeMs = 21;
    EXPECT_NE(window.poll(*ring).front().sequence, identity);
    ring->pumpWriteIndex = 0;
    EXPECT_TRUE(window.poll(*ring).empty());
}

TEST(PumpWindow, ReusedReservationDoesNotPublishPreviousSlotContent) {
    auto ring = std::make_unique<hook_ipc::Ring>();
    ring->pumpEnabled = 1;
    PumpWindow window;
    window.begin(*ring);
    ring->pumpWriteIndex = 1;
    strcpy_s(ring->pumpSlots[0].method, "GetBuffer");
    ring->pumpSlots[0].timeMs = 1;
    ASSERT_EQ(window.poll(*ring).size(), 1u);
    const auto old = window.poll(*ring).front().sequence;
    ring->pumpWriteIndex = 65;
    EXPECT_TRUE(window.poll(*ring).empty());
    ring->pumpSlots[0].timeMs = 65;
    ASSERT_EQ(window.poll(*ring).size(), 1u);
    EXPECT_NE(window.poll(*ring).front().sequence, old);
}

TEST(PumpWindow, PodStringsAndBlobNeverReadBeyondFixedArrays) {
    hook_ipc::CallPod pod{};
    std::memset(pod.iface, 'I', sizeof(pod.iface));
    std::memset(pod.method, 'M', sizeof(pod.method));
    std::memset(pod.args, 'A', sizeof(pod.args));
    pod.argBlobLen = 65535;
    const auto call = decodeHookedCallPod(pod);
    EXPECT_EQ(call.iface, std::string(40, 'I'));
    EXPECT_EQ(call.method, std::string(40, 'M'));
    EXPECT_EQ(call.args, std::string(192, 'A'));
    EXPECT_TRUE(call.parameters.empty());
}
