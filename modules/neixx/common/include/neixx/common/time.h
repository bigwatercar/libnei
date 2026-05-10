#pragma once

#ifndef NEI_COMMON_TIME_H
#define NEI_COMMON_TIME_H

#include <cstdint>

#include <nei/macros/nei_export.h>

namespace nei {

// TimeDelta represents a duration measured in microseconds.
// Non-PIMPL: sizeof(TimeDelta) == sizeof(int64_t).
class NEI_API TimeDelta {
public:
    constexpr TimeDelta() : delta_(0) {}

    // Factory methods.
    static constexpr TimeDelta FromDays(int64_t days) {
        return TimeDelta(days * kMicrosecondsPerDay);
    }
    static constexpr TimeDelta FromHours(int64_t hours) {
        return TimeDelta(hours * kMicrosecondsPerHour);
    }
    static constexpr TimeDelta FromMinutes(int64_t minutes) {
        return TimeDelta(minutes * kMicrosecondsPerMinute);
    }
    static constexpr TimeDelta FromSeconds(int64_t seconds) {
        return TimeDelta(seconds * kMicrosecondsPerSecond);
    }
    static constexpr TimeDelta FromMilliseconds(int64_t ms) {
        return TimeDelta(ms * kMicrosecondsPerMillisecond);
    }
    static constexpr TimeDelta FromMicroseconds(int64_t us) {
        return TimeDelta(us);
    }

    // Accessors.
    constexpr int64_t InDays() const {
        return delta_ / kMicrosecondsPerDay;
    }
    constexpr int64_t InHours() const {
        return delta_ / kMicrosecondsPerHour;
    }
    constexpr int64_t InMinutes() const {
        return delta_ / kMicrosecondsPerMinute;
    }
    constexpr int64_t InSeconds() const {
        return delta_ / kMicrosecondsPerSecond;
    }
    constexpr double InSecondsF() const {
        return static_cast<double>(delta_) / static_cast<double>(kMicrosecondsPerSecond);
    }
    constexpr int64_t InMilliseconds() const {
        return delta_ / kMicrosecondsPerMillisecond;
    }
    constexpr double InMillisecondsF() const {
        return static_cast<double>(delta_) / static_cast<double>(kMicrosecondsPerMillisecond);
    }
    constexpr int64_t InMicroseconds() const { return delta_; }

    constexpr bool is_zero() const {
        return delta_ == 0;
    }
    constexpr bool is_positive() const {
        return delta_ > 0;
    }
    constexpr bool is_negative() const {
        return delta_ < 0;
    }

    // Arithmetic operators.
    constexpr TimeDelta operator+(TimeDelta other) const {
        return TimeDelta(delta_ + other.delta_);
    }
    constexpr TimeDelta operator-(TimeDelta other) const {
        return TimeDelta(delta_ - other.delta_);
    }
    constexpr TimeDelta operator*(int64_t scalar) const {
        return TimeDelta(delta_ * scalar);
    }
    constexpr TimeDelta operator/(int64_t scalar) const {
        return TimeDelta(delta_ / scalar);
    }
    constexpr TimeDelta& operator+=(TimeDelta other) {
        delta_ += other.delta_;
        return *this;
    }
    constexpr TimeDelta& operator-=(TimeDelta other) {
        delta_ -= other.delta_;
        return *this;
    }

    // Comparison operators.
    constexpr bool operator==(TimeDelta other) const {
        return delta_ == other.delta_;
    }
    constexpr bool operator!=(TimeDelta other) const {
        return delta_ != other.delta_;
    }
    constexpr bool operator<(TimeDelta other) const {
        return delta_ < other.delta_;
    }
    constexpr bool operator<=(TimeDelta other) const {
        return delta_ <= other.delta_;
    }
    constexpr bool operator>(TimeDelta other) const {
        return delta_ > other.delta_;
    }
    constexpr bool operator>=(TimeDelta other) const {
        return delta_ >= other.delta_;
    }

private:
    static constexpr int64_t kMicrosecondsPerDay = 86'400'000'000LL;
    static constexpr int64_t kMicrosecondsPerHour = 3'600'000'000LL;
    static constexpr int64_t kMicrosecondsPerMinute = 60'000'000LL;
    static constexpr int64_t kMicrosecondsPerSecond = 1'000'000LL;
    static constexpr int64_t kMicrosecondsPerMillisecond = 1'000LL;

    explicit constexpr TimeDelta(int64_t delta) : delta_(delta) {}

    int64_t delta_;
};

static_assert(sizeof(TimeDelta) == sizeof(int64_t),
              "TimeDelta must be exactly sizeof(int64_t)");

// Scalar * TimeDelta (commutative).
inline constexpr TimeDelta operator*(int64_t scalar, TimeDelta delta) {
    return delta * scalar;
}

// ---------------------------------------------------------------------------

// Time represents a wall-clock timestamp measured in microseconds since Unix
// epoch (1970-01-01 00:00:00 UTC). Non-PIMPL: sizeof(Time) == sizeof(int64_t).
class NEI_API Time {
public:
    constexpr Time() : us_since_unix_epoch_(0) {}

    // Returns the current wall-clock time.
    // Windows: GetSystemTimeAsFileTime.
    // POSIX:   clock_gettime(CLOCK_REALTIME).
    static Time Now();

    static constexpr Time UnixEpoch() {
        return Time(0);
    }

    static constexpr Time FromUnixSeconds(int64_t seconds) {
        return Time(seconds * kMicrosecondsPerSecond);
    }
    static constexpr Time FromUnixMilliseconds(int64_t ms) {
        return Time(ms * kMicrosecondsPerMillisecond);
    }
    static constexpr Time FromUnixMicroseconds(int64_t us) {
        return Time(us);
    }

    constexpr int64_t ToUnixSeconds() const {
        return us_since_unix_epoch_ / kMicrosecondsPerSecond;
    }
    constexpr int64_t ToUnixMilliseconds() const {
        return us_since_unix_epoch_ / kMicrosecondsPerMillisecond;
    }
    constexpr int64_t ToUnixMicroseconds() const {
        return us_since_unix_epoch_;
    }

    constexpr Time operator+(TimeDelta delta) const {
        return Time(us_since_unix_epoch_ + delta.InMicroseconds());
    }
    constexpr Time operator-(TimeDelta delta) const {
        return Time(us_since_unix_epoch_ - delta.InMicroseconds());
    }
    constexpr Time &operator+=(TimeDelta delta) {
        us_since_unix_epoch_ += delta.InMicroseconds();
        return *this;
    }
    constexpr Time &operator-=(TimeDelta delta) {
        us_since_unix_epoch_ -= delta.InMicroseconds();
        return *this;
    }
    constexpr TimeDelta operator-(Time other) const {
        return TimeDelta::FromMicroseconds(us_since_unix_epoch_ - other.us_since_unix_epoch_);
    }

    constexpr bool operator==(Time other) const {
        return us_since_unix_epoch_ == other.us_since_unix_epoch_;
    }
    constexpr bool operator!=(Time other) const {
        return us_since_unix_epoch_ != other.us_since_unix_epoch_;
    }
    constexpr bool operator<(Time other) const {
        return us_since_unix_epoch_ < other.us_since_unix_epoch_;
    }
    constexpr bool operator<=(Time other) const {
        return us_since_unix_epoch_ <= other.us_since_unix_epoch_;
    }
    constexpr bool operator>(Time other) const {
        return us_since_unix_epoch_ > other.us_since_unix_epoch_;
    }
    constexpr bool operator>=(Time other) const {
        return us_since_unix_epoch_ >= other.us_since_unix_epoch_;
    }

private:
    static constexpr int64_t kMicrosecondsPerSecond = 1'000'000LL;
    static constexpr int64_t kMicrosecondsPerMillisecond = 1'000LL;

    explicit constexpr Time(int64_t us_since_unix_epoch)
        : us_since_unix_epoch_(us_since_unix_epoch) {}

    int64_t us_since_unix_epoch_;
};

static_assert(sizeof(Time) == sizeof(int64_t),
              "Time must be exactly sizeof(int64_t)");

// ---------------------------------------------------------------------------

// TimeTicks represents a monotonically increasing timestamp measured in
// microseconds. Non-PIMPL: sizeof(TimeTicks) == sizeof(int64_t).
class NEI_API TimeTicks {
public:
    constexpr TimeTicks() : ticks_(0) {}

    // Returns the current monotonic time.
    // Windows: QueryPerformanceCounter.
    // POSIX:   clock_gettime(CLOCK_MONOTONIC).
    static TimeTicks Now();

    // Raw microsecond value (relative to an unspecified epoch; only
    // differences between two TimeTicks values are meaningful).
    constexpr int64_t ToInternalValue() const { return ticks_; }

    // Returns true if this TimeTicks carries no meaningful timestamp.
    // By convention in this codebase, default-constructed TimeTicks is null.
    constexpr bool is_null() const { return ticks_ == 0; }

    // Arithmetic with TimeDelta.
    constexpr TimeTicks operator+(TimeDelta delta) const {
        return TimeTicks(ticks_ + delta.InMicroseconds());
    }
    constexpr TimeTicks operator-(TimeDelta delta) const {
        return TimeTicks(ticks_ - delta.InMicroseconds());
    }
    constexpr TimeTicks& operator+=(TimeDelta delta) {
        ticks_ += delta.InMicroseconds();
        return *this;
    }
    constexpr TimeTicks& operator-=(TimeDelta delta) {
        ticks_ -= delta.InMicroseconds();
        return *this;
    }

    // Difference between two TimeTicks yields a TimeDelta.
    constexpr TimeDelta operator-(TimeTicks other) const {
        return TimeDelta::FromMicroseconds(ticks_ - other.ticks_);
    }

    // Comparison operators.
    constexpr bool operator==(TimeTicks other) const {
        return ticks_ == other.ticks_;
    }
    constexpr bool operator!=(TimeTicks other) const {
        return ticks_ != other.ticks_;
    }
    constexpr bool operator<(TimeTicks other) const {
        return ticks_ < other.ticks_;
    }
    constexpr bool operator<=(TimeTicks other) const {
        return ticks_ <= other.ticks_;
    }
    constexpr bool operator>(TimeTicks other) const {
        return ticks_ > other.ticks_;
    }
    constexpr bool operator>=(TimeTicks other) const {
        return ticks_ >= other.ticks_;
    }

private:
    explicit constexpr TimeTicks(int64_t ticks) : ticks_(ticks) {}

    int64_t ticks_;
};

static_assert(sizeof(TimeTicks) == sizeof(int64_t),
              "TimeTicks must be exactly sizeof(int64_t)");

} // namespace nei

#endif // NEI_COMMON_TIME_H
