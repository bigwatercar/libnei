#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <nei/utils/flake_id.h>

#include <stddef.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <time.h>
#endif

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

#if defined(_WIN32)

static uint64_t nei_flake_wall_ms_win32(void) {
  FILETIME ft;
  ULARGE_INTEGER value;
  const uint64_t kFileTimeToUnixEpoch100ns = 116444736000000000ULL;

  GetSystemTimeAsFileTime(&ft);
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;

  if (value.QuadPart <= kFileTimeToUnixEpoch100ns) {
    return 0ULL;
  }
  return (value.QuadPart - kFileTimeToUnixEpoch100ns) / 10000ULL;
}

static uint64_t nei_flake_unix_ms_now_impl(void) {
  /*
   * Windows 7 only has GetSystemTimeAsFileTime (coarser than Win8+ precise API).
   * We anchor QueryPerformanceCounter (QPC) to wall time once, then use
   * max(wall_ms, qpc_projected_ms) to keep millisecond ticks monotonic and
   * resistant to coarse wall-clock granularity/regressions.
   */
  static volatile LONG s_qpc_state = 0; /* 0=uninit, 1=initing, 2=ready */
  static LARGE_INTEGER s_qpc_freq;
  static LARGE_INTEGER s_qpc_base;
  static uint64_t s_wall_base_ms = 0ULL;

  uint64_t wall_ms = nei_flake_wall_ms_win32();

  if (s_qpc_state != 2) {
    if (InterlockedCompareExchange(&s_qpc_state, 1, 0) == 0) {
      LARGE_INTEGER freq;
      LARGE_INTEGER now;
      if (QueryPerformanceFrequency(&freq) != 0 && QueryPerformanceCounter(&now) != 0 && freq.QuadPart > 0) {
        s_qpc_freq = freq;
        s_qpc_base = now;
        s_wall_base_ms = wall_ms;
        InterlockedExchange(&s_qpc_state, 2);
      } else {
        InterlockedExchange(&s_qpc_state, 0);
        return wall_ms;
      }
    } else {
      while (s_qpc_state == 1) {
        YieldProcessor();
      }
    }
  }

  if (s_qpc_state == 2) {
    LARGE_INTEGER now;
    if (QueryPerformanceCounter(&now) != 0) {
      const uint64_t elapsed_counts = (now.QuadPart >= s_qpc_base.QuadPart)
                                          ? (uint64_t)(now.QuadPart - s_qpc_base.QuadPart)
                                          : 0ULL;
      const uint64_t qpc_ms = s_wall_base_ms + (elapsed_counts * 1000ULL) / (uint64_t)s_qpc_freq.QuadPart;
      if (qpc_ms > wall_ms) {
        wall_ms = qpc_ms;
      }
    }
  }

  return wall_ms;
}

#else

static uint64_t nei_flake_unix_ms_now_impl(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 0ULL;
  }
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

#endif

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

uint64_t nei_flake_unix_ms_now(void) {
  return nei_flake_unix_ms_now_impl();
}

uint64_t nei_flake_next_id(void) {
  uint64_t now_ms;
  uint64_t ts_part;
  uint64_t id;

  if (s_tls.initialized == 0U) {
    s_tls.thread_tag = nei_flake_next_thread_tag();
    s_tls.last_ms = 0ULL;
    s_tls.sequence = 0U;
    s_tls.initialized = 1U;
  }

  now_ms = nei_flake_unix_ms_now_impl();
  if (now_ms < NEI_FLAKE_EPOCH_MS) {
    now_ms = NEI_FLAKE_EPOCH_MS;
  }

  if (now_ms < s_tls.last_ms) {
    now_ms = s_tls.last_ms;
  }

  if (now_ms == s_tls.last_ms) {
    if (s_tls.sequence >= (uint32_t)NEI_FLAKE_SEQUENCE_MASK) {
      do {
        now_ms = nei_flake_unix_ms_now_impl();
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
