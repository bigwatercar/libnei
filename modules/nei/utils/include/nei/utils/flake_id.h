#pragma once
#ifndef NEI_UTILS_FLAKE_ID_H
#define NEI_UTILS_FLAKE_ID_H

#include <nei/macros/nei_export.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 2024-01-01 00:00:00 UTC in Unix milliseconds. */
#define NEI_FLAKE_EPOCH_MS 1704067200000ULL

#define NEI_FLAKE_TIMESTAMP_BITS 41U
#define NEI_FLAKE_THREAD_TAG_BITS 5U
#define NEI_FLAKE_SEQUENCE_BITS 17U

#define NEI_FLAKE_SEQUENCE_MASK ((1ULL << NEI_FLAKE_SEQUENCE_BITS) - 1ULL)
#define NEI_FLAKE_THREAD_TAG_MASK ((1U << NEI_FLAKE_THREAD_TAG_BITS) - 1U)
#define NEI_FLAKE_TIMESTAMP_MASK ((1ULL << NEI_FLAKE_TIMESTAMP_BITS) - 1ULL)

/**
 * Generate a 64-bit flake id.
 *
 * Bit layout (high to low):
 *   [41-bit timestamp(ms since 2024-01-01 UTC)]
 *   [5-bit thread tag]
 *   [17-bit per-thread sequence]
 */
NEI_API uint64_t nei_flake_next_id(void);

/** Returns current wall-clock milliseconds since Unix epoch. */
NEI_API uint64_t nei_flake_unix_ms_now(void);

#ifdef __cplusplus
}
#endif

#endif /* NEI_UTILS_FLAKE_ID_H */
