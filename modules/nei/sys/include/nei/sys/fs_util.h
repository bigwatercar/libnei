#pragma once
#ifndef NEI_SYS_FS_UTIL_H
#define NEI_SYS_FS_UTIL_H

/*
 * File-system utility functions (cross-platform).
 *
 * All path arguments are UTF-8 encoded.  On Windows paths are converted
 * to UTF-16LE internally; on POSIX they are used directly.
 */

#include <nei/macros/nei_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check whether a file is currently in use by another process.
 *
 * On Windows this performs a mandatory exclusivity check (@c dwShareMode=0).
 * On POSIX this uses @c flock (advisory  --  only detects cooperative
 * lockers) and a write-open test (catches @c ETXTBSY on running
 * executables).  A return value of 0 does not guarantee the file is
 * truly idle on POSIX.
 *
 * @param path UTF-8 path to the file (must not be NULL).
 * @return Non-zero if the file appears to be busy, 0 if it is free,
 *         or a negative value on error (e.g. file not found).
 */
NEI_API int nei_is_file_busy(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NEI_SYS_FS_UTIL_H */
