#pragma once
#ifndef NEI_SYS_CPU_INFO_H
#define NEI_SYS_CPU_INFO_H

#include <nei/build/nei_export.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the number of logical CPU cores (including hyper-threads).
 *
 * This returns the number of hardware threads the OS scheduler can use,
 * which may be larger than the physical core count on CPUs with SMT/HT.
 *
 * @return Number of logical CPU cores on success, or a negative value on error.
 */
NEI_API int nei_get_cpu_count(void);

/**
 * @brief Get the number of physical CPU cores (packages × cores per package).
 *
 * On systems where physical core count cannot be determined, this function
 * falls back to returning the logical CPU count.
 *
 * @return Number of physical CPU cores on success, or a negative value on error.
 */
NEI_API int nei_get_cpu_physical_count(void);

/**
 * @brief Get the CPU architecture string.
 *
 * Returns a short identifier for the hardware architecture, e.g.
 * @"x86_64"@, @"aarch64"@, @"x86"@, @"arm"@.
 *
 * This is a runtime detection (e.g. via @c uname or @c sysctl), not a
 * compile-time macro, so it reflects the actual hardware even when
 * running under emulation/translation layers (e.g. Rosetta 2 on Apple Silicon).
 *
 * @param buf  Output buffer to receive the architecture string (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_cpu_arch(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <string>

/**
 * @brief Convenience wrapper that returns the CPU architecture as a std::string.
 */
inline std::string nei_get_cpu_arch() {
  char buf[64];
  int len = ::nei_get_cpu_arch(buf, sizeof(buf));
  if (len < 0) {
    return {};
  }
  return std::string(buf, static_cast<size_t>(len));
}

#endif

#endif /* NEI_SYS_CPU_INFO_H */
