#include "neixx/common/time.h"

#if defined(_WIN32)
#include <Windows.h>
#else
#include <time.h>
#endif

namespace nei {

// ---------------------------------------------------------------------------
// Time::Now()
// ---------------------------------------------------------------------------

#if defined(_WIN32)

Time Time::Now() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);

    ULARGE_INTEGER ticks;
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;

    // FILETIME is 100ns ticks since 1601-01-01.
    // Unix epoch offset in 100ns ticks: 11644473600s * 10,000,000.
    constexpr uint64_t kFileTimeToUnixEpoch100ns = 116444736000000000ULL;
    const uint64_t unix_100ns = ticks.QuadPart - kFileTimeToUnixEpoch100ns;
    const int64_t unix_us = static_cast<int64_t>(unix_100ns / 10ULL);
    return Time::FromUnixMicroseconds(unix_us);
}

#else // POSIX

Time Time::Now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const int64_t us =
        static_cast<int64_t>(ts.tv_sec) * 1'000'000LL +
        static_cast<int64_t>(ts.tv_nsec) / 1'000LL;
    return Time::FromUnixMicroseconds(us);
}

#endif // defined(_WIN32)

// ---------------------------------------------------------------------------
// TimeTicks::Now()
// ---------------------------------------------------------------------------

#if defined(_WIN32)

TimeTicks TimeTicks::Now() {
    // Cache the QPC frequency once; it is constant after system boot.
    static const LONGLONG freq = []() -> LONGLONG {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    // Convert to microseconds without overflow:
    //   us = (counter / freq) * 1e6 + (counter % freq) * 1e6 / freq
    const LONGLONG q = counter.QuadPart / freq;
    const LONGLONG r = counter.QuadPart % freq;
    const int64_t us = q * 1'000'000LL + r * 1'000'000LL / freq;

    return TimeTicks(us);
}

#else // POSIX

TimeTicks TimeTicks::Now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t us =
        static_cast<int64_t>(ts.tv_sec) * 1'000'000LL +
        static_cast<int64_t>(ts.tv_nsec) / 1'000LL;
    return TimeTicks(us);
}

#endif // defined(_WIN32)

} // namespace nei
