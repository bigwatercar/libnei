#pragma once
#ifndef NEI_CORE_FILE_UTIL_H
#define NEI_CORE_FILE_UTIL_H

/*
 * UTF-8 file path utilities (cross-platform).
 */

#include <nei/macros/nei_export.h>

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a file using a UTF-8 encoded path.
 *
 * On Windows the path is converted to UTF-16LE and @c _wfopen_s is used.
 * On POSIX systems the path is passed directly to @c fopen (paths are
 * already UTF-8 on modern systems).
 *
 * @param path UTF-8 file path (must not be NULL).
 * @param mode @c fopen mode string, e.g. @c "rb", @c "w" (must not be NULL).
 * @return @c FILE* on success, or @c NULL on failure.
 */
NEI_API FILE *nei_fopen_utf8(const char *path, const char *mode);

#ifdef __cplusplus
}
#endif

#endif /* NEI_CORE_FILE_UTIL_H */
