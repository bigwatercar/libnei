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
    constexpr int64_t InSeconds() const {
        return delta_ / kMicrosecondsPerSecond;
    }
    constexpr int64_t InMilliseconds() const {
        return delta_ / kMicrosecondsPerMillisecond;
    }
    constexpr int64_t InMicroseconds() const { return delta_; }

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
