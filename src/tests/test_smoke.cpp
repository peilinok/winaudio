#include <gtest/gtest.h>
#include "Result.h"

TEST(Smoke, ResultOkIsTruthy) {
    wa::Result r = wa::Result::Ok();
    EXPECT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r.code, 0);
}

TEST(Smoke, ResultFailCarriesMessage) {
    wa::Result r = wa::Result::Fail(-1, "boom");
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_EQ(r.message, "boom");
}
