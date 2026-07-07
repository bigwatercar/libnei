#include <nei/core/time.h>

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

int64_t nei_time_now_ms_hires(void) {
    return nei_time_now_ms();
}

int64_t nei_time_now_us_hires(void) {
    return nei_time_now_us();
}

void nei_time_qpc_ensure_anchor(void) {
    /* POSIX: clock_gettime(CLOCK_REALTIME) already has nanosecond
       precision  --  no separate anchor initialization needed. */
}

int64_t nei_time_qpc_fast_us(void) {
    return nei_time_now_us();
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
