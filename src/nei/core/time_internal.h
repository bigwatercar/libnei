// Internal header  --  NOT part of the public API.
//
// Provides direct access to the QPC-anchored wall-clock for
// performance-critical callers within the nei library.  External
// consumers should use the public nei_time_now_*_hires() API.

#pragma once
#ifndef NEI_CORE_TIME_INTERNAL_H
#define NEI_CORE_TIME_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Ensures the QPC anchor is initialized (idempotent, thread-safe).
// Must be called at least once before nei_time_qpc_fast_us().
void nei_time_qpc_ensure_anchor(void);

// QPC-anchored wall-clock microseconds since Unix epoch.
// Returns a negative value on error (QPC unavailable).
// Requires: nei_time_qpc_ensure_anchor() has been called at least once.
int64_t nei_time_qpc_fast_us(void);

#ifdef __cplusplus
}
#endif

#endif // NEI_CORE_TIME_INTERNAL_H
