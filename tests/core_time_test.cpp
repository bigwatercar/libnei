#include <gtest/gtest.h>

#include <cstdint>

#include <nei/core/time.h>

/* =========================================================================
 * Wall-clock
 * ========================================================================= */

TEST(CoreTimeTest, NowSecReturnsPositive) {
  int64_t t = nei_time_now_sec();
  EXPECT_GT(t, 0) << "Unix timestamp should be > 0";
}

TEST(CoreTimeTest, NowMsReturnsPositive) {
  int64_t t = nei_time_now_ms();
  EXPECT_GT(t, 0);
}

TEST(CoreTimeTest, NowUsReturnsPositive) {
  int64_t t = nei_time_now_us();
  EXPECT_GT(t, 0);
}

TEST(CoreTimeTest, NowConsistency) {
  int64_t sec = nei_time_now_sec();
  int64_t ms = nei_time_now_ms();
  int64_t us = nei_time_now_us();

  EXPECT_NEAR((double)sec, (double)ms / 1000.0, 1.0);
  EXPECT_NEAR((double)ms, (double)us / 1000.0, 10.0);
}

/* =========================================================================
 * Monotonic
 * ========================================================================= */

TEST(CoreTimeTest, MonotonicMsReturnsPositive) {
  int64_t t = nei_time_monotonic_ms();
  EXPECT_GT(t, 0);
}

TEST(CoreTimeTest, MonotonicUsReturnsPositive) {
  int64_t t = nei_time_monotonic_us();
  EXPECT_GT(t, 0);
}

TEST(CoreTimeTest, MonotonicNeverGoesBackwards) {
  int64_t t1 = nei_time_monotonic_us();
  volatile int x = 0;
  for (int i = 0; i < 100000; ++i)
    x += i;
  (void)x;
  int64_t t2 = nei_time_monotonic_us();
  EXPECT_GE(t2, t1) << "Monotonic clock must never go backwards";
}

TEST(CoreTimeTest, MonotonicMsUsConsistency) {
  int64_t ms = nei_time_monotonic_ms();
  int64_t us = nei_time_monotonic_us();
  EXPECT_NEAR((double)ms, (double)us / 1000.0, 1.0);
}
