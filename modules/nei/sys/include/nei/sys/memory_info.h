#pragma once
#ifndef NEI_SYS_MEMORY_INFO_H
#define NEI_SYS_MEMORY_INFO_H

#include <nei/build/nei_export.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the total physical memory size in bytes.
 *
 * @return Total physical memory in bytes on success, or 0 on error.
 */
NEI_API uint64_t nei_get_total_physical_memory(void);

/**
 * @brief Get the available physical memory size in bytes.
 *
 * The definition of "available" is platform-dependent:
 * - On Windows, this is the amount of physical memory available to the
 *   process (not necessarily free  --  it includes memory that can be
 *   reclaimed from caches and stand-by lists).
 * - On macOS, this is the free + inactive memory reported by the VM
 *   statistics subsystem.
 * - On Linux, this is MemAvailable as reported by @c /proc/meminfo,
 *   which represents an estimate of how much memory is available for
 *   starting new applications without swapping.
 *
 * @return Available physical memory in bytes on success, or 0 on error.
 */
NEI_API uint64_t nei_get_available_physical_memory(void);

/**
 * @brief Get the system page size in bytes.
 *
 * @return Page size in bytes on success, or a negative value on error.
 */
NEI_API int nei_get_page_size(void);

#ifdef __cplusplus
}
#endif

#endif /* NEI_SYS_MEMORY_INFO_H */
