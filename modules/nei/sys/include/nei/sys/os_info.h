#pragma once
#ifndef NEI_SYS_OS_INFO_H
#define NEI_SYS_OS_INFO_H

#include <nei/macros/nei_export.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the operating system name.
 *
 * Returns a short, human-readable name such as @"Windows"@, @"macOS"@,
 * or @"Linux"@.
 *
 * @param buf  Output buffer to receive the OS name (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_os_name(char *buf, size_t size);

/**
 * @brief Get the operating system version string.
 *
 * The format is platform-dependent:
 * - Windows: e.g. @"10.0.19045"@ (major.minor.build)
 * - macOS:   e.g. @"14.5"@ (from kern.osproductversion)
 * - Linux:   e.g. contents of @c /etc/os-release @c VERSION_ID field,
 *            or the kernel version as a fallback.
 *
 * @param buf  Output buffer to receive the version string (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_os_version(char *buf, size_t size);

/**
 * @brief Get the kernel version string.
 *
 * - Windows: the NT kernel version reported by @c RtlGetVersion(), e.g.
 *   @"10.0.19045"@.
 * - macOS: @c kern.osrelease, e.g. @"23.5.0"@.
 * - Linux: @c uname -r output, e.g. @"6.5.0-14-generic"@.
 *
 * @param buf  Output buffer to receive the kernel version (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_kernel_version(char *buf, size_t size);

/**
 * @brief Detect whether the current process is running under WSL
 *        (Windows Subsystem for Linux).
 *
 * On Linux, checks \c /proc/version for a \c "Microsoft" or \c "WSL"
 * signature.  On all other platforms, always returns 0 (false).
 *
 * @return 1 if running inside WSL, 0 otherwise.
 */
NEI_API int nei_is_running_on_wsl(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <string>

inline std::string nei_get_os_name() {
    char buf[128];
    int len = ::nei_get_os_name(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(len));
}

inline std::string nei_get_os_version() {
    char buf[128];
    int len = ::nei_get_os_version(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(len));
}

inline std::string nei_get_kernel_version() {
    char buf[128];
    int len = ::nei_get_kernel_version(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(len));
}

#endif

#endif /* NEI_SYS_OS_INFO_H */
