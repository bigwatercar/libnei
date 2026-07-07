#define _CRT_SECURE_NO_WARNINGS
#include <nei/sys/os_info.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

/* MSVC in C mode may not expose snprintf; map to _snprintf. */
#if defined(_MSC_VER) && !defined(__cplusplus)
#define snprintf _snprintf
#endif
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#else /* Linux / other Unix */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#endif

int nei_get_os_name(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }

    const char *name = NULL;

#ifdef _WIN32
    name = "Windows";
#elif defined(__APPLE__)
    name = "macOS";
#else
    name = "Linux";
#endif

    size_t len = strlen(name);
    if (len >= size) {
        memcpy(buf, name, size - 1);
        buf[size - 1] = '\0';
        return (int)len;
    }
    memcpy(buf, name, len + 1);
    return (int)len;
}

int nei_get_os_version(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }

#ifdef _WIN32
    {
        /*
         * Use RtlGetVersion to get the true OS version (GetVersionEx is
         * subject to application manifest-based lies on Windows 8.1+).
         */
        typedef LONG(NTAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll == NULL) {
            return -1;
        }
        RtlGetVersionPtr rtl_get_version =
            (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
        if (rtl_get_version == NULL) {
            return -1;
        }

        RTL_OSVERSIONINFOW vi;
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (rtl_get_version(&vi) != 0) {
            return -1;
        }

        char result[64];
        int len = 0;
        if (vi.szCSDVersion[0] != L'\0') {
            len = snprintf(result, sizeof(result), "%lu.%lu.%lu %ls",
                           vi.dwMajorVersion, vi.dwMinorVersion,
                           vi.dwBuildNumber, vi.szCSDVersion);
        } else {
            len = snprintf(result, sizeof(result), "%lu.%lu.%lu",
                           vi.dwMajorVersion, vi.dwMinorVersion,
                           vi.dwBuildNumber);
        }
        if (len < 0 || (size_t)len >= sizeof(result)) {
            return -1;
        }

        if ((size_t)len >= size) {
            memcpy(buf, result, size - 1);
            buf[size - 1] = '\0';
            return len;
        }
        memcpy(buf, result, (size_t)len + 1);
        return len;
    }
#elif defined(__APPLE__)
    {
        /*
         * Use kern.osproductversion on macOS 10.11+ (e.g. "14.5").
         * Fall back to constructing from kern.osrelease (e.g. "23.5.0"
         * for macOS 14.5) if the product version is not available.
         */
        char version[64];
        size_t len = sizeof(version);
        if (sysctlbyname("kern.osproductversion", version, &len, NULL, 0) == 0 &&
            len > 0 && len <= sizeof(version)) {
            version[len - 1] = '\0'; /* Ensure null termination */
            size_t slen = strlen(version);
            if (slen >= size) {
                memcpy(buf, version, size - 1);
                buf[size - 1] = '\0';
                return (int)slen;
            }
            memcpy(buf, version, slen + 1);
            return (int)slen;
        }

        /* Fallback: construct from kern.osrelease (e.g. "23.5.0" -> "14.5") */
        len = sizeof(version);
        if (sysctlbyname("kern.osrelease", version, &len, NULL, 0) != 0) {
            return -1;
        }
        version[len - 1] = '\0';

        int major = 0, minor = 0;
        if (sscanf(version, "%d.%d", &major, &minor) != 2) {
            return -1;
        }

        /*
         * Map Darwin kernel version to macOS version.
         * macOS 10.x: major - 4
         * macOS 11+:  major - 9 (approximately)
         * For simplicity, we return the raw kernel version as fallback.
         */
        char result[64];
        int rlen = snprintf(result, sizeof(result), "%d.%d", major, minor);
        if (rlen < 0) {
            return -1;
        }

        if ((size_t)rlen >= size) {
            memcpy(buf, result, size - 1);
            buf[size - 1] = '\0';
            return rlen;
        }
        memcpy(buf, result, (size_t)rlen + 1);
        return rlen;
    }
#else
    {
        /*
         * Try /etc/os-release first for the VERSION_ID field.
         * Fall back to uname -r (kernel version).
         */
        FILE *f = fopen("/etc/os-release", "r");
        if (f != NULL) {
            char line[256];
            char version[128] = "";
            while (fgets(line, sizeof(line), f) != NULL) {
                char value[128];
                if (sscanf(line, "VERSION_ID=%127s", value) == 1) {
                    /* Strip surrounding quotes if present. */
                    size_t vlen = strlen(value);
                    if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
                        vlen -= 2;
                        memmove(value, value + 1, vlen);
                        value[vlen] = '\0';
                    }
                    strncpy(version, value, sizeof(version) - 1);
                    version[sizeof(version) - 1] = '\0';
                    break;
                }
            }
            fclose(f);

            if (version[0] != '\0') {
                size_t vlen = strlen(version);
                if (vlen >= size) {
                    memcpy(buf, version, size - 1);
                    buf[size - 1] = '\0';
                    return (int)vlen;
                }
                memcpy(buf, version, vlen + 1);
                return (int)vlen;
            }
        }

        /* Fallback: uname -r */
        struct utsname u;
        if (uname(&u) != 0) {
            return -1;
        }
        size_t rlen = strlen(u.release);
        if (rlen >= size) {
            memcpy(buf, u.release, size - 1);
            buf[size - 1] = '\0';
            return (int)rlen;
        }
        memcpy(buf, u.release, rlen + 1);
        return (int)rlen;
    }
#endif
}

int nei_get_kernel_version(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }

#ifdef _WIN32
    {
        /* Use the same RtlGetVersion approach as nei_get_os_version. */
        typedef LONG(NTAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll == NULL) {
            return -1;
        }
        RtlGetVersionPtr rtl_get_version =
            (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
        if (rtl_get_version == NULL) {
            return -1;
        }

        RTL_OSVERSIONINFOW vi;
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (rtl_get_version(&vi) != 0) {
            return -1;
        }

        char result[64];
        int len = snprintf(result, sizeof(result), "%lu.%lu.%lu",
                           vi.dwMajorVersion, vi.dwMinorVersion,
                           vi.dwBuildNumber);
        if (len < 0 || (size_t)len >= sizeof(result)) {
            return -1;
        }

        if ((size_t)len >= size) {
            memcpy(buf, result, size - 1);
            buf[size - 1] = '\0';
            return len;
        }
        memcpy(buf, result, (size_t)len + 1);
        return len;
    }
#elif defined(__APPLE__)
    {
        char osrelease[64];
        size_t len = sizeof(osrelease);
        if (sysctlbyname("kern.osrelease", osrelease, &len, NULL, 0) != 0) {
            return -1;
        }
        osrelease[len - 1] = '\0'; /* Ensure null termination */
        size_t slen = strlen(osrelease);
        if (slen >= size) {
            memcpy(buf, osrelease, size - 1);
            buf[size - 1] = '\0';
            return (int)slen;
        }
        memcpy(buf, osrelease, slen + 1);
        return (int)slen;
    }
#else
    {
        struct utsname u;
        if (uname(&u) != 0) {
            return -1;
        }
        size_t rlen = strlen(u.release);
        if (rlen >= size) {
            memcpy(buf, u.release, size - 1);
            buf[size - 1] = '\0';
            return (int)rlen;
        }
        memcpy(buf, u.release, rlen + 1);
        return (int)rlen;
    }
#endif
}
