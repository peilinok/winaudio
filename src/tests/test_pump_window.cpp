#include <gtest/gtest.h>
#include "PumpWindow.h"
#include <memory>

using namespace wa;

TEST(PumpWindow, ExcludesOldSlotsAndOnlyAcceptsCompletePublications) {
    auto ring = std::make_unique<hook_ipc::Ring>();
    hook_ipc::initializeRing(*ring);
    ring->pumpEnabled = 1;
    hook_ipc::CallPod pod{};
    pod.pump = 1;
    strcpy_s(pod.method, "GetBuffer");
    pod.timeMs = 10;
    ASSERT_TRUE(hook_ipc::writeCall(*ring, pod));
    PumpWindow window;
    window.begin(*ring);
    EXPECT_TRUE(window.poll(*ring).empty());
    // A diagnostic reservation index is never a publication guarantee.
    ++ring->pumpWriteIndex;
    EXPECT_TRUE(window.poll(*ring).empty());
    strcpy_s(pod.method, "ReleaseBuffer");
    pod.timeMs = 20;
    ASSERT_TRUE(hook_ipc::writeCall(*ring, pod));
    const auto first = window.poll(*ring);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first.front().call.method, "ReleaseBuffer");
    const auto identity = first.front().sequence;
    EXPECT_EQ(window.poll(*ring).front().sequence, identity);
    ++ring->transportEpoch;
    EXPECT_TRUE(window.transportStatus(*ring).unknown);
}

TEST(PumpWindow, ReusedSlotHasOneStableIdentityPerCommittedRecord) {
    auto ring = std::make_unique<hook_ipc::Ring>();
    hook_ipc::initializeRing(*ring);
    ring->pumpEnabled = 1;
    PumpWindow window;
    window.begin(*ring);
    hook_ipc::CallPod pod{};
    pod.pump = 1;
    strcpy_s(pod.method, "GetBuffer");
    pod.timeMs = 1;
    ASSERT_TRUE(hook_ipc::writeCall(*ring, pod));
    ASSERT_EQ(window.poll(*ring).size(), 1u);
    const auto old = window.poll(*ring).front().sequence;
    for (int i = 2; i <= 66; ++i) {
        pod.timeMs = i;
        ASSERT_TRUE(hook_ipc::writeCall(*ring, pod));
    }
    const auto current = window.poll(*ring);
    ASSERT_EQ(current.size(), 64u);
    for (const auto& call : current) {
        EXPECT_NE(call.sequence, old);
        EXPECT_GE(call.call.timeMs, 3);
    }
    EXPECT_EQ(window.transportStatus(*ring).overwritten, 1u);
    EXPECT_EQ(window.poll(*ring).back().sequence, current.back().sequence);
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
