#pragma once
#ifndef NEI_CORE_TIME_H
#define NEI_CORE_TIME_H

/*
 * Platform-agnostic timestamp utilities.
 *
 * Wall-clock time is based on the Unix epoch (1970-01-01 00:00:00 UTC)
 * and may be affected by system clock adjustments.  Use monotonic time
 * for interval measurement.
 */

#include <stdint.h>

#include <nei/build/nei_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Wall-clock time (Unix epoch based)
 * ========================================================================= */

/**
 * @brief Current Unix timestamp in seconds.
 *
 * @return Seconds since 1970-01-01 00:00:00 UTC, or negative on error.
 */
NEI_API int64_t nei_time_now_sec(void);

/**
 * @brief Current Unix timestamp in milliseconds (fast, ~ms resolution).
 *
 * Uses the lightest available system call.  On Windows this is a
 * user-mode shared-memory read (~1-2 ns); on POSIX it is clock_gettime.
 * The returned value advances in ~1-16 ms increments depending on the
 * system timer resolution.  For sub-millisecond precision use
 * nei_time_now_ms_hires().
 *
 * @return Milliseconds since Unix epoch, or negative on error.
 */
NEI_API int64_t nei_time_now_ms(void);

/**
 * @brief Current Unix timestamp in microseconds (fast, ~ms resolution).
 *
 * @copydetails nei_time_now_ms()
 * @return Microseconds since Unix epoch, or negative on error.
 */
NEI_API int64_t nei_time_now_us(void);

/**
 * @brief High-resolution Unix timestamp in milliseconds.
 *
 * Uses QPC-based anchoring to provide sub-millisecond precision.
 * Slightly more expensive per call (~10-15 ns) but advances
 * continuously rather than in timer ticks.  Prefer this when
 * generating IDs, measuring short intervals, or any scenario
 * where coarse timer granularity would be visible.
 *
 * @return Milliseconds since Unix epoch, or negative on error.
 */
NEI_API int64_t nei_time_now_ms_hires(void);

/**
 * @brief High-resolution Unix timestamp in microseconds.
 *
 * @copydetails nei_time_now_ms_hires()
 * @return Microseconds since Unix epoch, or negative on error.
 */
NEI_API int64_t nei_time_now_us_hires(void);

/* =========================================================================
 * Monotonic time (for interval measurement)
 * ========================================================================= */

/**
 * @brief Monotonic time in milliseconds.
 *
 * This clock never goes backwards and is unaffected by system clock
 * adjustments.  The absolute value is meaningless  --  only differences
 * between two calls within the same process lifetime are valid.
 *
 * @return Monotonic time in ms, or negative on error.
 */
NEI_API int64_t nei_time_monotonic_ms(void);

/**
 * @brief Monotonic time in microseconds.
 *
 * @return Monotonic time in μs, or negative on error.
 */
NEI_API int64_t nei_time_monotonic_us(void);

#ifdef __cplusplus
}
#endif

#endif /* NEI_CORE_TIME_H */
