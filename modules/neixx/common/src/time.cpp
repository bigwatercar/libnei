#include "neixx/common/time.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <time.h>
#endif

namespace nei {

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
