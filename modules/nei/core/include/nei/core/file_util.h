#pragma once
#ifndef NEI_CORE_FILE_UTIL_H
#define NEI_CORE_FILE_UTIL_H

/*
 * UTF-8 file path utilities (cross-platform).
 */

#include <nei/build/nei_export.h>

#include <stdint.h>
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

/**
 * @brief Check whether a file or directory exists.
 *
 * @param path UTF-8 path (must not be NULL).
 * @return Non-zero (true) if the path exists, 0 (false) otherwise.
 */
NEI_API int nei_file_exists(const char *path);

/**
 * @brief Get the size of a regular file in bytes.
 *
 * @param path     UTF-8 path (must not be NULL).
 * @param out_size Receives the file size in bytes (must not be NULL).
 * @return 0 on success, or a negative value on error (e.g. path is a
 *         directory or does not exist).
 */
NEI_API int nei_file_size(const char *path, uint64_t *out_size);

/**
 * @brief Delete a file.
 *
 * @param path UTF-8 path (must not be NULL).
 * @return 0 on success, or a negative value on error.
 */
NEI_API int nei_file_remove(const char *path);

/**
 * @brief Rename or move a file or directory.
 *
 * On Windows this uses @c _wrename which may fail when moving across
 * different volumes.  Use OS-specific APIs for cross-volume moves.
 *
 * @param old_path UTF-8 path to the existing file (must not be NULL).
 * @param new_path UTF-8 path to the new location (must not be NULL).
 * @return 0 on success, or a negative value on error.
 */
NEI_API int nei_file_rename(const char *old_path, const char *new_path);

/**
 * @brief Truncate (clear) a file to zero length.  Creates the file if it
 * does not exist.
 *
 * @param path UTF-8 path.
 * @return 0 on success, or a negative value on error.
 */
NEI_API int nei_file_truncate(const char *path);

/**
 * @brief Append data to the end of a file.  Creates the file if it does
 * not exist.
 *
 * @param path UTF-8 path.
 * @param buf  Data to write.
 * @param len  Number of bytes to write.
 * @return Number of bytes written, or a negative value on error.
 */
NEI_API int64_t nei_file_append(const char *path, const void *buf, size_t len);

/**
 * @brief Overwrite a file with the given data.  Any existing content is
 * replaced.  Creates the file if it does not exist.
 *
 * @param path UTF-8 path.
 * @param buf  Data to write.
 * @param len  Number of bytes to write.
 * @return Number of bytes written, or a negative value on error.
 */
NEI_API int64_t nei_file_write(const char *path, const void *buf, size_t len);

/**
 * @brief Read data from a file at the given offset.
 *
 * Convenience for reading small files: call nei_file_size() first to
 * allocate a buffer, then read the entire file in one call.
 *
 * @param path   UTF-8 path.
 * @param offset Byte offset to start reading from.
 * @param buf    Buffer to receive the data.
 * @param len    Maximum number of bytes to read.
 * @return The number of bytes actually read (>=0), or a negative value
 *         on error (file not found, seek/read failure, etc.).
 */
NEI_API int64_t nei_file_read(const char *path, int64_t offset, void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NEI_CORE_FILE_UTIL_H */
