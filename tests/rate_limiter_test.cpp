#include <gtest/gtest.h>

#include <cstdint>

#include "crawler/politeness.h"

TEST(MemoryRateLimiterTest, StrictSpacingWithBurstOne) {
    int64_t now = 1000;
    mc::MemoryRateLimiter rl([&] { return now; });
    EXPECT_EQ(rl.acquire("h", 10, 1), 0);    // 新桶是满的
    EXPECT_EQ(rl.acquire("h", 10, 1), 100);  // 10 个/秒 -> 要等 100ms
    now += 50;
    EXPECT_EQ(rl.acquire("h", 10, 1), 50);   // 等了一半，还差 50ms（并且上一次失败没有消耗令牌）
    now += 50;
    EXPECT_EQ(rl.acquire("h", 10, 1), 0);
}

TEST(MemoryRateLimiterTest, BurstAllowsQuickSuccession) {
    int64_t now = 0;
    mc::MemoryRateLimiter rl([&] { return now; });
    EXPECT_EQ(rl.acquire("h", 10, 3), 0);
    EXPECT_EQ(rl.acquire("h", 10, 3), 0);
    EXPECT_EQ(rl.acquire("h", 10, 3), 0);
    EXPECT_EQ(rl.acquire("h", 10, 3), 100);
}

TEST(MemoryRateLimiterTest, RefillIsCappedAtBurst) {
    int64_t now = 0;
    mc::MemoryRateLimiter rl([&] { return now; });
    for (int i = 0; i < 3; ++i) rl.acquire("h", 10, 3);
    now += 3600 * 1000;  // 闲置一小时也只能攒满 3 个，不会攒出几万个
    EXPECT_EQ(rl.acquire("h", 10, 3), 0);
    EXPECT_EQ(rl.acquire("h", 10, 3), 0);
    EXPECT_EQ(rl.acquire("h", 10, 3), 0);
    EXPECT_GT(rl.acquire("h", 10, 3), 0);
}

TEST(MemoryRateLimiterTest, HostsAreIndependent) {
    int64_t now = 0;
    mc::MemoryRateLimiter rl([&] { return now; });
    EXPECT_EQ(rl.acquire("a:80", 1, 1), 0);
    EXPECT_GT(rl.acquire("a:80", 1, 1), 0);
    EXPECT_EQ(rl.acquire("b:80", 1, 1), 0);
}

TEST(MemoryRateLimiterTest, NonPositiveRateMeansUnlimited) {
    mc::MemoryRateLimiter rl;
    for (int i = 0; i < 100; ++i) EXPECT_EQ(rl.acquire("h", 0, 1), 0);
}

TEST(MemoryRobotsStoreTest, SaveLoadAndExpiry) {
    mc::MemoryRobotsStore s;
    std::string blob;
    EXPECT_FALSE(s.load("h:80", &blob));
    s.save("h:80", "Tabc", 1);
    ASSERT_TRUE(s.load("h:80", &blob));
    EXPECT_EQ(blob, "Tabc");
}