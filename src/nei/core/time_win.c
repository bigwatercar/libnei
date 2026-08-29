#include <nei/core/time.h>

#include <windows.h>

/* -------------------------------------------------------------------------
 * Internal: cached performance counter frequency.
 * ------------------------------------------------------------------------- */
static int64_t get_qpc_freq(void) {
  static int64_t cached = 0;
  if (cached == 0) {
    LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq))
      return -1;
    cached = (int64_t)freq.QuadPart;
  }
  return cached;
}

/* -------------------------------------------------------------------------
 * Wall-clock (FILETIME -> Unix epoch)
 * ------------------------------------------------------------------------- */

/* FILETIME epoch (1601) to Unix epoch (1970) offset in 100-ns units. */
#define NEI_TIME_FILETIME_UNIX_EPOCH 116444736000000000ULL

static int64_t filetime_to_unix(DWORD low, DWORD high, uint64_t divisor) {
  ULARGE_INTEGER ul;
  ul.LowPart = low;
  ul.HighPart = high;
  return (int64_t)((ul.QuadPart - NEI_TIME_FILETIME_UNIX_EPOCH) / divisor);
}

/* -------------------------------------------------------------------------
 * QPC-anchored wall-clock for nei_time_now_{ms,us}_hires().
 *
 * GetSystemTimeAsFileTime reads from a shared user page (~1-2 ns) but
 * only advances in ~1-16 ms increments (system timer granularity).  For
 * callers that need continuous sub-millisecond timestamps (e.g. flake-id
 * generation, short-interval measurement), we anchor the wall clock to
 * the high-resolution performance counter and project forward in user
 * mode, yielding microsecond-level precision at ~10-15 ns per call.
 *
 * The anchor is re-established periodically (~100 ms) to correct for the
 * tiny long-term drift between QPC and system time.  Reads of the anchored
 * state use plain loads after the one-time initialization barrier; occasional
 * tearing of anchor_qpc / anchor_wall_us during re-anchor is benign because
 * the projection error is bounded by one re-anchor interval (< 1 ms).
 * ------------------------------------------------------------------------- */

typedef struct nei_time_qpc_anchor_st {
  int64_t wall_us;
  int64_t qpc;
  int64_t freq;
  volatile LONG state; /* 0=uninit, 1=init-in-progress, 2=ready */
} nei_time_qpc_anchor_st;

static nei_time_qpc_anchor_st g_qpc_anchor = {0, 0, 0, 0};

#define NEI_TIME_QPC_REANCHOR_INTERVAL_US 100000

/* GetSystemTimePreciseAsFileTime (Win8+)  --  provides microsecond-precision
 * wall-clock time directly, avoiding the need for QPC anchoring. */
static VOID(WINAPI *g_pfn_GetSystemTimePreciseAsFileTime)(LPFILETIME) = NULL;
static volatile LONG g_precise_detected = 0;

static void nei_time_detect_precise_api(void) {
  if (InterlockedCompareExchange(&g_precise_detected, 1, 0) == 0) {
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (h != NULL) {
      g_pfn_GetSystemTimePreciseAsFileTime =
          (VOID(WINAPI *)(LPFILETIME))GetProcAddress(h, "GetSystemTimePreciseAsFileTime");
    }
  }
}

static void nei_time_qpc_init_anchor(void) {
  if (InterlockedCompareExchange(&g_qpc_anchor.state, 1, 0) == 0) {
    nei_time_detect_precise_api();

    if (g_pfn_GetSystemTimePreciseAsFileTime != NULL) {
      /* Precise API available  --  no QPC anchoring needed. */
      InterlockedExchange(&g_qpc_anchor.state, 2);
      return;
    }

    LARGE_INTEGER qpc = {0};
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    const int64_t freq = get_qpc_freq();
    if (freq > 0 && QueryPerformanceCounter(&qpc)) {
      g_qpc_anchor.freq = freq;
      g_qpc_anchor.qpc = (int64_t)qpc.QuadPart;
      g_qpc_anchor.wall_us = filetime_to_unix(ft.dwLowDateTime, ft.dwHighDateTime, 10ULL);
      InterlockedExchange(&g_qpc_anchor.state, 2);
    } else {
      InterlockedExchange(&g_qpc_anchor.state, 0);
    }
  } else {
    while (g_qpc_anchor.state == 1) {
      YieldProcessor();
    }
  }
}

int64_t nei_time_qpc_fast_us(void) {
  /* Win8+: use GetSystemTimePreciseAsFileTime directly  --  no QPC anchoring. */
  if (g_pfn_GetSystemTimePreciseAsFileTime != NULL) {
    FILETIME ft;
    g_pfn_GetSystemTimePreciseAsFileTime(&ft);
    return filetime_to_unix(ft.dwLowDateTime, ft.dwHighDateTime, 10ULL);
  }

  /* Fallback: QPC-anchored projection (Win7 / Server 2008 R2). */
  LARGE_INTEGER qpc;
  if (!QueryPerformanceCounter(&qpc)) {
    return -1;
  }

  const int64_t anchor_wall = g_qpc_anchor.wall_us;
  const int64_t anchor_qpc = g_qpc_anchor.qpc;
  const int64_t freq = g_qpc_anchor.freq;

  const int64_t qpc_now = (int64_t)qpc.QuadPart;
  const int64_t elapsed = (qpc_now >= anchor_qpc) ? (qpc_now - anchor_qpc) : 0;
  const int64_t delta_us = (elapsed * 1000000LL) / freq;
  const int64_t projected_us = anchor_wall + delta_us;

  if (delta_us >= NEI_TIME_QPC_REANCHOR_INTERVAL_US) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const int64_t fresh_wall_us = filetime_to_unix(ft.dwLowDateTime, ft.dwHighDateTime, 10ULL);
    g_qpc_anchor.wall_us = fresh_wall_us;
    g_qpc_anchor.qpc = qpc_now;
  }

  return projected_us;
}

void nei_time_qpc_ensure_anchor(void) {
  if (g_qpc_anchor.state != 2) {
    nei_time_qpc_init_anchor();
  }
}

/* =========================================================================
 * Fast path (GetSystemTimeAsFileTime / shared-user-page)
 * ========================================================================= */

int64_t nei_time_now_sec(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return filetime_to_unix(ft.dwLowDateTime, ft.dwHighDateTime, 10000000ULL);
}

int64_t nei_time_now_ms(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return filetime_to_unix(ft.dwLowDateTime, ft.dwHighDateTime, 10000ULL);
}

int64_t nei_time_now_us(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return filetime_to_unix(ft.dwLowDateTime, ft.dwHighDateTime, 10ULL);
}

/* =========================================================================
 * High-resolution path (QPC-anchored)
 * ========================================================================= */

int64_t nei_time_now_ms_hires(void) {
  if (g_qpc_anchor.state != 2) {
    nei_time_qpc_init_anchor();
  }
  if (g_qpc_anchor.state == 2) {
    const int64_t fast_us = nei_time_qpc_fast_us();
    if (fast_us >= 0)
      return fast_us / 1000;
  }
  return nei_time_now_ms();
}

int64_t nei_time_now_us_hires(void) {
  if (g_qpc_anchor.state != 2) {
    nei_time_qpc_init_anchor();
  }
  if (g_qpc_anchor.state == 2) {
    const int64_t fast_us = nei_time_qpc_fast_us();
    if (fast_us >= 0)
      return fast_us;
  }
  return nei_time_now_us();
}

/* -------------------------------------------------------------------------
 * Monotonic (QPC)
 * ------------------------------------------------------------------------- */

static int64_t qpc_to_units(uint64_t multiplier) {
  LARGE_INTEGER counter;
  int64_t freq = get_qpc_freq();
  if (freq <= 0 || !QueryPerformanceCounter(&counter))
    return -1;
  return (int64_t)(((uint64_t)counter.QuadPart * multiplier) / (uint64_t)freq);
}

int64_t nei_time_monotonic_ms(void) {
  return qpc_to_units(1000ULL);
}

int64_t nei_time_monotonic_us(void) {
  return qpc_to_units(1000000ULL);
}
