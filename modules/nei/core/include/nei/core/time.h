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

#include <nei/macros/nei_export.h>

#include <stdint.h>

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
 * @brief Current Unix timestamp in milliseconds.
 *
 * @return Milliseconds since Unix epoch, or negative on error.
 */
NEI_API int64_t nei_time_now_ms(void);

/**
 * @brief Current Unix timestamp in microseconds.
 *
 * @return Microseconds since Unix epoch, or negative on error.
 */
NEI_API int64_t nei_time_now_us(void);

/* =========================================================================
 * Monotonic time (for interval measurement)
 * ========================================================================= */

/**
 * @brief Monotonic time in milliseconds.
 *
 * This clock never goes backwards and is unaffected by system clock
 * adjustments.  The absolute value is meaningless — only differences
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
