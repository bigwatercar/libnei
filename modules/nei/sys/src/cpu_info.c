#include <nei/sys/cpu_info.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else /* Linux / other Unix */
#include <sys/utsname.h>
#include <unistd.h>
#include <stdio.h>
#endif
#include <stdlib.h>

int nei_get_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(__APPLE__)
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) == 0) {
        return ncpu;
    }
    /* Fallback: hw.ncpu */
    if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0) {
        return ncpu;
    }
    return -1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 0) {
        return -1;
    }
    return (int)n;
#endif
}

int nei_get_cpu_physical_count(void) {
#ifdef _WIN32
    /*
     * Use GetLogicalProcessorInformation to count physical cores.
     * Count entries with RelationProcessorCore for total physical cores.
     * If that fails, fall back to the logical count.
     */
    DWORD buf_size = 0;
    GetLogicalProcessorInformation(NULL, &buf_size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return nei_get_cpu_count();
    }

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info =
        (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(buf_size);
    if (info == NULL) {
        return nei_get_cpu_count();
    }

    DWORD count = 0;
    if (GetLogicalProcessorInformation(info, &buf_size)) {
        DWORD entry_count = buf_size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        for (DWORD i = 0; i < entry_count; ++i) {
            if (info[i].Relationship == RelationProcessorCore) {
                ++count;
            }
        }
    }

    free(info);

    if (count == 0) {
        return nei_get_cpu_count();
    }
    return (int)count;
#elif defined(__APPLE__)
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.physicalcpu", &ncpu, &len, NULL, 0) == 0) {
        return ncpu;
    }
    /* Fallback: hw.physicalcpu_max */
    if (sysctlbyname("hw.physicalcpu_max", &ncpu, &len, NULL, 0) == 0) {
        return ncpu;
    }
    return nei_get_cpu_count();
#else
    /*
     * On Linux, parse /proc/cpuinfo and count unique (physical id, core id)
     * pairs.  If that fails, fall back to logical count.
     */
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) {
        return nei_get_cpu_count();
    }

    /* Simple approach: count lines matching "cpu cores" and use the max value,
     * then multiply by the number of unique "physical id" values.
     * For a robust implementation we'd track pairs; for a simple one we
     * just return the logical count as fallback for now. */
    int logical = nei_get_cpu_count();
    int cores_per_socket = 0;
    int sockets = 0;
    int last_phys_id = -1;
    char line[256];

    while (fgets(line, sizeof(line), f) != NULL) {
        int phy_id;
        if (sscanf(line, "physical id : %d", &phy_id) == 1) {
            if (phy_id != last_phys_id) {
                last_phys_id = phy_id;
                ++sockets;
            }
        }
        int cpu_cores;
        if (sscanf(line, "cpu cores : %d", &cpu_cores) == 1) {
            if (cpu_cores > cores_per_socket) {
                cores_per_socket = cpu_cores;
            }
        }
    }
    fclose(f);

    if (sockets > 0 && cores_per_socket > 0) {
        int physical = sockets * cores_per_socket;
        if (physical > 0 && physical <= logical) {
            return physical;
        }
    }

    /* Fallback: try _SC_NPROCESSORS_CONF (which may still be logical on x86) */
    return logical;
#endif
}

int nei_get_cpu_arch(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }

#ifdef _WIN32
    {
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        const char *arch = "unknown";
        switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            arch = "x86_64";
            break;
        case PROCESSOR_ARCHITECTURE_ARM64:
            arch = "aarch64";
            break;
        case PROCESSOR_ARCHITECTURE_INTEL:
            arch = "x86";
            break;
        case PROCESSOR_ARCHITECTURE_ARM:
            arch = "arm";
            break;
        case PROCESSOR_ARCHITECTURE_IA64:
            arch = "ia64";
            break;
        default:
            break;
        }
        size_t len = strlen(arch);
        if (len >= size) {
            memcpy(buf, arch, size - 1);
            buf[size - 1] = '\0';
            return (int)len;
        }
        memcpy(buf, arch, len + 1);
        return (int)len;
    }
#elif defined(__APPLE__)
    {
        char machine[64];
        size_t len = sizeof(machine);
        if (sysctlbyname("hw.machine", machine, &len, NULL, 0) != 0) {
            return -1;
        }
        /* Normalize common machine names to architecture strings. */
        const char *arch = "unknown";
        if (strncmp(machine, "x86_64", 6) == 0) {
            arch = "x86_64";
        } else if (strncmp(machine, "arm64", 5) == 0) {
            arch = "aarch64";
        } else if (strncmp(machine, "i386", 4) == 0) {
            arch = "x86";
        } else if (strncmp(machine, "arm", 3) == 0 && machine[3] != '6') {
            arch = "arm";
        } else {
            /* Return the raw machine name as-is. */
            arch = machine;
            len = strlen(machine);
        }
        len = strlen(arch);
        if (len >= size) {
            memcpy(buf, arch, size - 1);
            buf[size - 1] = '\0';
            return (int)len;
        }
        memcpy(buf, arch, len + 1);
        return (int)len;
    }
#else
    {
        struct utsname u;
        if (uname(&u) != 0) {
            return -1;
        }
        const char *machine = u.machine;
        /* Normalize common Linux machine strings. */
        const char *arch = machine;
        if (strcmp(machine, "x86_64") == 0 || strcmp(machine, "amd64") == 0) {
            arch = "x86_64";
        } else if (strcmp(machine, "aarch64") == 0 || strcmp(machine, "arm64") == 0) {
            arch = "aarch64";
        } else if (strcmp(machine, "i686") == 0 || strcmp(machine, "i386") == 0 ||
                   strcmp(machine, "i586") == 0) {
            arch = "x86";
        } else if (strncmp(machine, "armv", 4) == 0) {
            arch = "arm";
        }
        size_t len = strlen(arch);
        if (len >= size) {
            memcpy(buf, arch, size - 1);
            buf[size - 1] = '\0';
            return (int)len;
        }
        memcpy(buf, arch, len + 1);
        return (int)len;
    }
#endif
}
