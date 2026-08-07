#pragma once
#ifndef NEI_CORE_PATH_UTIL_H
#define NEI_CORE_PATH_UTIL_H

/*
 * UTF-8 path string manipulation and file-system query utilities.
 *
 * String operations (dir name, base name, extension, normalize, join, …)
 * are pure string manipulation and work on all platforms.
 *
 * File-system queries (exists, is_file, is_dir, permissions, …) access
 * the file system and have platform-specific implementations.
 */

#include <nei/build/nei_export.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * String operations (cross-platform, no file-system access)
 * ========================================================================= */

/**
 * @brief Return non-zero if @p c is a path separator.
 *
 * Recognises both @c '/' and @c '\\' on all platforms.
 */
NEI_API int nei_path_is_separator(char c);

/**
 * @brief Return non-zero if @p path is an absolute path.
 *
 * - POSIX:   starts with @c '/'.
 * - Windows: starts with @c '\\', @c '/', or @c "[A-Za-z]:".
 */
NEI_API int nei_path_is_absolute(const char *path);

/**
 * @brief Join two path components.
 *
 * If @p b is an absolute path it replaces @p a entirely.
 *
 * @param dst  Output buffer (must not be NULL).
 * @param size Size of @p dst in bytes.
 * @param a    First path component (must not be NULL).
 * @param b    Second path component (must not be NULL).
 * @return Number of bytes written (excluding null), or required size
 *         if @p dst is too small.  Negative on error.
 */
NEI_API int nei_path_join(char *dst, size_t size, const char *a, const char *b);

/**
 * @brief Extract the directory portion of @p path.
 *
 * Returns everything before the last path separator.  If no separator
 * is found, returns @c ".".
 *
 * @param path UTF-8 path (must not be NULL).
 * @param buf  Output buffer (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return Number of bytes written (excluding null), or required size
 *         if @p buf is too small.  Negative on error.
 */
NEI_API int nei_path_dirname(const char *path, char *buf, size_t size);

/**
 * @brief Extract the final component of @p path (the "base name").
 *
 * Returns everything after the last path separator, including any
 * extensions.
 *
 * @param path UTF-8 path (must not be NULL).
 * @param buf  Output buffer (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return Number of bytes written (excluding null), or required size
 *         if @p buf is too small.  Negative on error.
 */
NEI_API int nei_path_basename(const char *path, char *buf, size_t size);

/**
 * @brief Extract the file name without any extension ("stem").
 *
 * Removes all extensions from the base name.  For @c "archive.tar.gz"
 * this returns @c "archive"; for @c "file.txt" it returns @c "file".
 *
 * @param path UTF-8 path (must not be NULL).
 * @param buf  Output buffer (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return Number of bytes written (excluding null), or required size
 *         if @p buf is too small.  Negative on error.
 */
NEI_API int nei_path_stem(const char *path, char *buf, size_t size);

/**
 * @brief Extract the last extension (including the leading dot).
 *
 * For @c "archive.tar.gz" this returns @c ".gz".
 * Returns an empty string if there is no extension or if the base name
 * starts with a dot.
 *
 * @param path UTF-8 path (must not be NULL).
 * @param buf  Output buffer (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return Number of bytes written (excluding null), or required size
 *         if @p buf is too small.  Negative on error.
 */
NEI_API int nei_path_extension(const char *path, char *buf, size_t size);

/**
 * @brief Extract all extensions (including the leading dot).
 *
 * For @c "archive.tar.gz" this returns @c ".tar.gz".
 * Returns an empty string if there is no extension or if the base name
 * starts with a dot.
 *
 * @param path UTF-8 path (must not be NULL).
 * @param buf  Output buffer (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return Number of bytes written (excluding null), or required size
 *         if @p buf is too small.  Negative on error.
 */
NEI_API int nei_path_extensions(const char *path, char *buf, size_t size);

/**
 * @brief Normalise a path by resolving @c "." and @c ".." components.
 *
 * Removes redundant separators and resolves relative components where
 * possible.  This is a purely lexical operation and does not access
 * the file system.
 *
 * @param path UTF-8 path (must not be NULL).
 * @param buf  Output buffer (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return Number of bytes written (excluding null), or required size
 *         if @p buf is too small.  Negative on error.
 */
NEI_API int nei_path_normalize(const char *path, char *buf, size_t size);

/**
 * @brief Convert path separators to the platform-native form.
 *
 * On Windows this replaces @c '/' with @c '\\' (Win32 APIs accept both,
 * but @c '\\' is the canonical form).
 *
 * On POSIX systems this is a no-op  --  backslash is a valid filename
 * character and must not be altered.
 *
 * @param path UTF-8 path (must not be NULL).  Modified in place.
 * @return The number of characters in the resulting string
 *         (excluding null).  Negative on error.
 */
NEI_API int nei_path_to_native(char *path);

/* =========================================================================
 * File-system queries (platform-specific)
 * ========================================================================= */

/**
 * @brief Check whether @p path exists (file, directory, or other).
 *
 * @param path UTF-8 path (must not be NULL).
 * @return Non-zero if the path exists, 0 otherwise.
 */
NEI_API int nei_path_exists(const char *path);

/**
 * @brief Check whether @p path is a regular file.
 *
 * @param path UTF-8 path (must not be NULL).
 * @return Non-zero if @p path exists and is a regular file.
 */
NEI_API int nei_path_is_file(const char *path);

/**
 * @brief Check whether @p path is a directory.
 *
 * @param path UTF-8 path (must not be NULL).
 * @return Non-zero if @p path exists and is a directory.
 */
NEI_API int nei_path_is_dir(const char *path);

/**
 * @brief Check whether @p path is readable by the current process.
 *
 * @param path UTF-8 path (must not be NULL).
 * @return Non-zero if readable, 0 if not or on error.
 */
NEI_API int nei_path_is_readable(const char *path);

/**
 * @brief Check whether @p path is writable by the current process.
 *
 * @param path UTF-8 path (must not be NULL).
 * @return Non-zero if writable, 0 if not or on error.
 */
NEI_API int nei_path_is_writable(const char *path);

/**
 * @brief Check whether @p path is executable by the current process.
 *
 * On Windows this checks whether the file name ends in an executable
 * extension (@c .exe, @c .bat, @c .cmd, @c .com).
 *
 * @param path UTF-8 path (must not be NULL).
 * @return Non-zero if executable, 0 if not or on error.
 */
NEI_API int nei_path_is_executable(const char *path);

/* =========================================================================
 * File-system operations (platform-specific)
 * ========================================================================= */

/**
 * @brief Create a directory.
 *
 * If @p parents is non-zero, intermediate directories are created as
 * needed (similar to @c mkdir -p).
 *
 * @param path    UTF-8 path (must not be NULL).
 * @param parents Non-zero to create parent directories automatically.
 * @return 0 on success, or a negative value on error.
 */
NEI_API int nei_path_create_dir(const char *path, int parents);

/**
 * @brief Remove a file or directory.
 *
 * If @p recursive is non-zero, directories are removed recursively
 * (including all contents).  Non-recursive removal of a non-empty
 * directory will fail.
 *
 * @param path      UTF-8 path (must not be NULL).
 * @param recursive Non-zero to remove directories recursively.
 * @return 0 on success, or a negative value on error.
 */
NEI_API int nei_path_remove(const char *path, int recursive);

#ifdef __cplusplus
}
#endif

#endif /* NEI_CORE_PATH_UTIL_H */
