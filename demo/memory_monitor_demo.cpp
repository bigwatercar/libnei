/**
 * @file memory_monitor_demo.cpp
 * @brief Demonstrates process and system memory monitoring using nei/sys APIs.
 *
 * Checks:
 *  1. Process memory usage  --  warns if resident memory exceeds a threshold.
 *  2. System memory pressure  --  warns if available memory drops too low.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <nei/sys/memory_info.h>
#include <nei/sys/process_info.h>

namespace {

/* Thresholds  --  tweak as needed for your application. */
constexpr uint64_t kProcessResidentWarnBytes = 100ULL * 1024 * 1024; /* 100 MiB */
constexpr double   kSystemMemoryWarnRatio    = 0.10;               /* < 10 % free */

void print_separator() { std::printf("----------------------------------------\n"); }

void print_process_memory() {
    nei_process_memory_info_st info;
    if (nei_get_process_memory_info(&info) != 0) {
        std::printf("[ERROR] Failed to get process memory info.\n");
        return;
    }

    std::printf("--- Process Memory ---\n");
    std::printf("  Resident   : %10llu KiB  (%llu MiB)\n",
                (unsigned long long)(info.resident_bytes / 1024ULL),
                (unsigned long long)(info.resident_bytes / (1024ULL * 1024ULL)));
    std::printf("  Peak RSS   : %10llu KiB\n",
                (unsigned long long)(info.peak_resident_bytes / 1024ULL));
    std::printf("  Virtual    : %10llu KiB\n",
                (unsigned long long)(info.virtual_bytes / 1024ULL));
    std::printf("  Peak VSize : %10llu KiB\n",
                (unsigned long long)(info.peak_virtual_bytes / 1024ULL));

    if (info.resident_bytes > kProcessResidentWarnBytes) {
        std::printf("  [WARN] Process resident memory exceeds %llu MiB!\n",
                    (unsigned long long)(kProcessResidentWarnBytes / (1024 * 1024)));
    } else {
        std::printf("  [OK]   Process memory is within limits.\n");
    }
}

void print_system_memory() {
    uint64_t total = nei_get_total_physical_memory();
    uint64_t avail = nei_get_available_physical_memory();
    if (total == 0 || avail == 0) {
        std::printf("[ERROR] Failed to get system memory info.\n");
        return;
    }

    uint64_t used = total - avail;
    double used_pct  = 100.0 * (double)used  / (double)total;
    double avail_pct = 100.0 * (double)avail / (double)total;

    std::printf("--- System Memory ---\n");
    std::printf("  Total      : %10llu MiB\n",
                (unsigned long long)(total / (1024ULL * 1024ULL)));
    std::printf("  Used       : %10llu MiB  (%5.1f %%)\n",
                (unsigned long long)(used / (1024ULL * 1024ULL)), used_pct);
    std::printf("  Available  : %10llu MiB  (%5.1f %%)\n",
                (unsigned long long)(avail / (1024ULL * 1024ULL)), avail_pct);

    double avail_ratio = (double)avail / (double)total;
    if (avail_ratio < kSystemMemoryWarnRatio) {
        std::printf("  [WARN] System memory critically low (< %.0f %% free)!\n",
                    kSystemMemoryWarnRatio * 100.0);
    } else if (avail_ratio < 0.20) {
        std::printf("  [NOTE] System memory is getting low (< 20 %% free).\n");
    } else {
        std::printf("  [OK]   System memory is healthy.\n");
    }
}

}  // namespace

int main() {
    std::printf("=== nei Memory Monitor Demo ===\n\n");

    print_process_memory();
    print_separator();
    print_system_memory();

    std::printf("\nDone.\n");
    return 0;
}
