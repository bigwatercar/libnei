#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <nei/utils/flake_id.h>

#include <stddef.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <nei/core/time.h>

#include "time_internal.h"

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#define NEI_FLAKE_HAS_C11_ATOMICS 1
#else
#define NEI_FLAKE_HAS_C11_ATOMICS 0
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define NEI_FLAKE_TLS _Thread_local
#elif defined(_MSC_VER)
#define NEI_FLAKE_TLS __declspec(thread)
#else
#define NEI_FLAKE_TLS __thread
#endif

#if NEI_FLAKE_HAS_C11_ATOMICS
static atomic_uint_least32_t s_thread_tag_counter = 0;
#elif defined(_WIN32)
static volatile LONG s_thread_tag_counter = 0;
#else
static volatile uint32_t s_thread_tag_counter = 0;
#endif

typedef struct nei_flake_tls_state_st {
  uint32_t initialized;
  uint32_t thread_tag;
  uint64_t last_ms;
  uint32_t sequence;
} nei_flake_tls_state_st;

static NEI_FLAKE_TLS nei_flake_tls_state_st s_tls = {0U, 0U, 0ULL, 0U};

/// Thin wrapper: calls the QPC-anchored fast path directly (bypassing the
/// DLL-export thunk).  The anchor is initialized lazily in the per-thread
/// TLS setup path above, so this hot path only does the QPC projection.
static uint64_t nei_flake_now_ms(void) {
  const int64_t us = nei_time_qpc_fast_us();
  if (us >= 0) {
    return (uint64_t)(us / 1000);
  }
  /* Fallback: QPC unavailable  --  use the regular wall clock. */
  const int64_t ms = nei_time_now_ms();
  return (ms >= 0) ? (uint64_t)ms : 0ULL;
}

#if NEI_FLAKE_HAS_C11_ATOMICS
static uint32_t nei_flake_next_thread_tag(void) {
  const uint32_t slot = (uint32_t)atomic_fetch_add_explicit(&s_thread_tag_counter, 1U, memory_order_relaxed);
  return slot & NEI_FLAKE_THREAD_TAG_MASK;
}
#elif defined(_WIN32)
static uint32_t nei_flake_next_thread_tag(void) {
  const LONG value = InterlockedIncrement(&s_thread_tag_counter) - 1;
  return (uint32_t)value & NEI_FLAKE_THREAD_TAG_MASK;
}
#else
static uint32_t nei_flake_next_thread_tag(void) {
  const uint32_t slot = __sync_fetch_and_add(&s_thread_tag_counter, 1U);
  return slot & NEI_FLAKE_THREAD_TAG_MASK;
}
#endif

uint64_t nei_flake_next_id(void) {
  uint64_t now_ms;
  uint64_t ts_part;
  uint64_t id;

  if (s_tls.initialized == 0U) {
    s_tls.thread_tag = nei_flake_next_thread_tag();
    s_tls.last_ms = 0ULL;
    s_tls.sequence = 0U;
    s_tls.initialized = 1U;
    nei_time_qpc_ensure_anchor(); /* once per process, idempotent */
  }

  now_ms = nei_flake_now_ms();
  if (now_ms < NEI_FLAKE_EPOCH_MS) {
    now_ms = NEI_FLAKE_EPOCH_MS;
  }

  if (now_ms < s_tls.last_ms) {
    now_ms = s_tls.last_ms;
  }

  if (now_ms == s_tls.last_ms) {
    if (s_tls.sequence >= (uint32_t)NEI_FLAKE_SEQUENCE_MASK) {
      do {
        now_ms = nei_flake_now_ms();
        if (now_ms < s_tls.last_ms) {
          now_ms = s_tls.last_ms;
        }
      } while (now_ms <= s_tls.last_ms);
      s_tls.last_ms = now_ms;
      s_tls.sequence = 0U;
    } else {
      ++s_tls.sequence;
    }
  } else {
    s_tls.last_ms = now_ms;
    s_tls.sequence = 0U;
  }

  ts_part = (s_tls.last_ms - NEI_FLAKE_EPOCH_MS) & NEI_FLAKE_TIMESTAMP_MASK;
  id = (ts_part << (NEI_FLAKE_THREAD_TAG_BITS + NEI_FLAKE_SEQUENCE_BITS))
       | ((uint64_t)(s_tls.thread_tag & NEI_FLAKE_THREAD_TAG_MASK) << NEI_FLAKE_SEQUENCE_BITS)
       | (uint64_t)(s_tls.sequence & (uint32_t)NEI_FLAKE_SEQUENCE_MASK);
  return id;
}
