#include <nei/core/time.h>

#ifdef _WIN32
#include <windows.h>

/* -------------------------------------------------------------------------
 * Internal: cached performance counter frequency.
 * ------------------------------------------------------------------------- */
static int64_t get_qpc_freq(void) {
    static int64_t cached = 0;
    if (cached == 0) {
        LARGE_INTEGER freq;
        if (!QueryPerformanceFrequency(&freq)) return -1;
        cached = (int64_t)freq.QuadPart;
    }
    return cached;
}

/* -------------------------------------------------------------------------
 * Wall-clock (FILETIME → Unix epoch)
 * ------------------------------------------------------------------------- */

/* FILETIME epoch (1601) to Unix epoch (1970) offset in 100-ns units. */
#define NEI_TIME_FILETIME_UNIX_EPOCH 116444736000000000ULL

static int64_t filetime_to_unix(DWORD low, DWORD high, uint64_t divisor) {
    ULARGE_INTEGER ul;
    ul.LowPart  = low;
    ul.HighPart = high;
    return (int64_t)((ul.QuadPart - NEI_TIME_FILETIME_UNIX_EPOCH) / divisor);
}

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

/* -------------------------------------------------------------------------
 * Monotonic (QPC)
 * ------------------------------------------------------------------------- */

static int64_t qpc_to_units(uint64_t multiplier) {
    LARGE_INTEGER counter;
    int64_t freq = get_qpc_freq();
    if (freq <= 0 || !QueryPerformanceCounter(&counter)) return -1;
    return (int64_t)(((uint64_t)counter.QuadPart * multiplier) /
                     (uint64_t)freq);
}

int64_t nei_time_monotonic_ms(void) { return qpc_to_units(1000ULL); }
int64_t nei_time_monotonic_us(void) { return qpc_to_units(1000000ULL); }

#else /* POSIX */

#include <time.h>

int64_t nei_time_now_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec;
}

int64_t nei_time_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int64_t nei_time_now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

int64_t nei_time_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int64_t nei_time_monotonic_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

#endif
