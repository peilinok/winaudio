#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#include "RuntimeFault.h"

using namespace wa;

namespace {
constexpr long kDeviceInvalidated = static_cast<long>(static_cast<int32_t>(0x88890004u)); // AUDCLNT_E_DEVICE_INVALIDATED
constexpr long kServiceNotRunning = static_cast<long>(static_cast<int32_t>(0x88890010u)); // AUDCLNT_E_SERVICE_NOT_RUNNING
} // namespace

TEST(RuntimeFault, StartsHealthy) {
    RuntimeFault f;
    EXPECT_FALSE(f.failed());
    EXPECT_EQ(f.code(), 0);
}

TEST(RuntimeFault, SuccessCodesAreNeverLatched) {
    RuntimeFault f;
    EXPECT_FALSE(f.report(0));   // S_OK
    EXPECT_FALSE(f.report(1));   // S_FALSE
    EXPECT_FALSE(f.failed());
    EXPECT_EQ(f.code(), 0);
}

TEST(RuntimeFault, FirstFailureWins) {
    RuntimeFault f;
    EXPECT_TRUE(f.report(kDeviceInvalidated));
    EXPECT_FALSE(f.report(kServiceNotRunning));
    EXPECT_TRUE(f.failed());
    EXPECT_EQ(f.code(), kDeviceInvalidated);
}

TEST(RuntimeFault, ResetAllowsANewFault) {
    RuntimeFault f;
    ASSERT_TRUE(f.report(kDeviceInvalidated));
    f.reset();
    EXPECT_FALSE(f.failed());
    EXPECT_TRUE(f.report(kServiceNotRunning));
    EXPECT_EQ(f.code(), kServiceNotRunning);
}

TEST(RuntimeFault, ExactlyOneConcurrentReporterLatches) {
    RuntimeFault f;
    std::atomic<int> winners{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
        ts.emplace_back([&, i] { if (f.report(-1 - i)) winners.fetch_add(1); });
    for (auto& t : ts) t.join();
    EXPECT_EQ(winners.load(), 1);
    EXPECT_TRUE(f.failed());
}
