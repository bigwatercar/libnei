#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <neixx/common/time.h>
#include <neixx/common/time_source.h>

namespace {

TEST(TimeDeltaTest, FromAndInUnitsRoundTripWorks) {
  using nei::TimeDelta;

  const TimeDelta one_day = TimeDelta::FromDays(1);
  EXPECT_EQ(one_day.InHours(), 24);
  EXPECT_EQ(one_day.InMinutes(), 24 * 60);
  EXPECT_EQ(one_day.InSeconds(), 24 * 60 * 60);

  const TimeDelta one_hour = TimeDelta::FromHours(1);
  EXPECT_EQ(one_hour.InMinutes(), 60);
  EXPECT_EQ(one_hour.InMilliseconds(), 3'600'000);

  const TimeDelta one_minute = TimeDelta::FromMinutes(1);
  EXPECT_EQ(one_minute.InSeconds(), 60);
  EXPECT_EQ(one_minute.InMicroseconds(), 60'000'000);
}

TEST(TimeDeltaTest, FloatingPointAccessorsProvideExpectedValues) {
  using nei::TimeDelta;

  const TimeDelta delta = TimeDelta::FromMicroseconds(1'500);
  EXPECT_DOUBLE_EQ(delta.InMillisecondsF(), 1.5);
  EXPECT_DOUBLE_EQ(delta.InSecondsF(), 0.0015);

  const TimeDelta precise = TimeDelta::FromMicroseconds(123'456);
  EXPECT_NEAR(precise.InMillisecondsF(), 123.456, 1e-9);
  EXPECT_NEAR(precise.InSecondsF(), 0.123456, 1e-12);
}

TEST(TimeDeltaTest, SignHelpersWorkForPositiveNegativeAndZero) {
  using nei::TimeDelta;

  const TimeDelta zero = TimeDelta::FromMicroseconds(0);
  const TimeDelta positive = TimeDelta::FromMilliseconds(1);
  const TimeDelta negative = TimeDelta::FromMilliseconds(-1);

  EXPECT_TRUE(zero.is_zero());
  EXPECT_FALSE(zero.is_positive());
  EXPECT_FALSE(zero.is_negative());

  EXPECT_FALSE(positive.is_zero());
  EXPECT_TRUE(positive.is_positive());
  EXPECT_FALSE(positive.is_negative());

  EXPECT_FALSE(negative.is_zero());
  EXPECT_FALSE(negative.is_positive());
  EXPECT_TRUE(negative.is_negative());
}

TEST(TimeTicksTest, NowIsMonotonicNonDecreasing) {
  nei::TimeTicks previous = nei::TimeTicks::Now();
  for (int i = 0; i < 64; ++i) {
    const nei::TimeTicks current = nei::TimeTicks::Now();
    EXPECT_GE(current, previous);
    previous = current;
  }
}

TEST(TimeSourceTest, SystemTimeSourceReturnsComparableTicks) {
  const nei::TimeTicks before = nei::TimeTicks::Now();
  const nei::TimeTicks from_source = nei::SystemTimeSource::Instance().Now();
  const nei::TimeTicks after = nei::TimeTicks::Now();

  EXPECT_GE(from_source, before);
  EXPECT_LE(from_source, after);

  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const nei::TimeTicks next = nei::SystemTimeSource::Instance().Now();
  EXPECT_GE(next, from_source);
}

TEST(TimeTest, UnixRoundTripConversionsWork) {
  const nei::Time t_sec = nei::Time::FromUnixSeconds(123);
  EXPECT_EQ(t_sec.ToUnixSeconds(), 123);
  EXPECT_EQ(t_sec.ToUnixMilliseconds(), 123'000);

  const nei::Time t_ms = nei::Time::FromUnixMilliseconds(456'789);
  EXPECT_EQ(t_ms.ToUnixMilliseconds(), 456'789);
  EXPECT_EQ(t_ms.ToUnixSeconds(), 456);

  const nei::Time t_us = nei::Time::FromUnixMicroseconds(987'654'321);
  EXPECT_EQ(t_us.ToUnixMicroseconds(), 987'654'321);
}

TEST(TimeTest, ArithmeticWithTimeDeltaWorks) {
  const nei::Time base = nei::Time::FromUnixSeconds(1'000);
  const nei::Time later = base + nei::TimeDelta::FromMilliseconds(250);
  EXPECT_EQ((later - base).InMilliseconds(), 250);

  nei::Time adjusted = later;
  adjusted -= nei::TimeDelta::FromMilliseconds(250);
  EXPECT_EQ(adjusted, base);
}

TEST(TimeTest, NowIsNonDecreasing) {
  const nei::Time before = nei::Time::Now();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const nei::Time after = nei::Time::Now();
  EXPECT_GE(after, before);
}

} // namespace
