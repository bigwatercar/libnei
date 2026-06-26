#pragma once
#ifndef NEI_CORE_ENCODING_H
#define NEI_CORE_ENCODING_H

/*
 * Character encoding conversion utilities.
 *
 * These functions are only available on Windows (wchar_t is UTF-16LE).
 * On non-Windows platforms including this header will not provide any
 * declarations, and calling these functions will produce a compile-time
 * error ("implicit declaration" / "undeclared identifier").
 */

#ifdef _WIN32

#include <nei/macros/nei_export.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Wide string (UTF-16LE)  ↔  UTF-8
 * ========================================================================= */

/**
 * @brief Convert a wide string to a UTF-8 encoded char buffer.
 *
 * @param src     Wide string (must not be NULL).
 * @param src_len Number of wchar_t in @p src, or -1 if @p src is
 *                null-terminated (in which case the null terminator
 *                is NOT included in the byte count returned).
 * @param buf     Output buffer to receive UTF-8 bytes (must not be NULL).
 * @param size    Size of @p buf in bytes.
 * @return On success, the number of UTF-8 bytes written (excluding the
 *         null terminator).  If @p buf is too small, the content is
 *         truncated, null-terminated, and the required buffer size is
 *         returned as a positive value.  On error, returns a negative
 *         value.
 */
NEI_API int nei_wstr_to_utf8(const wchar_t *src, int src_len,
                             char *buf, size_t size);

/**
 * @brief Convert a null-terminated UTF-8 string to a wide string buffer.
 *
 * @param src  UTF-8 string (must not be NULL, null-terminated).
 * @param buf  Output wide buffer (must not be NULL).
 * @param size Size of @p buf in wchar_t.
 * @return On success, the number of wchar_t written (excluding null).
 *         Returns a negative value if the conversion fails or if
 *         @p buf is too small to hold the result.
 */
NEI_API int nei_utf8_to_wstr(const char *src, wchar_t *buf, int size);

/* =========================================================================
 * MBCS (system ANSI code page)  ↔  UTF-8
 * ========================================================================= */

/**
 * @brief Convert a string from the system's ANSI code page (MBCS) to UTF-8.
 *
 * This is a two-step conversion via UTF-16LE.  The ANSI code page is
 * determined by the system locale (e.g. CP1252, CP932, CP936, CP949).
 *
 * @param src     MBCS string (must not be NULL).
 * @param src_len Byte length of @p src, or -1 if @p src is null-terminated.
 * @param buf     Output buffer to receive UTF-8 bytes (must not be NULL).
 * @param size    Size of @p buf in bytes.
 * @return On success, the number of UTF-8 bytes written (excluding the
 *         null terminator).  If @p buf is too small, the content is
 *         truncated, null-terminated, and the required buffer size is
 *         returned as a positive value.  On error, returns a negative
 *         value.
 */
NEI_API int nei_mbcs_to_utf8(const char *src, int src_len,
                             char *buf, size_t size);

/**
 * @brief Convert a UTF-8 string to the system's ANSI code page (MBCS).
 *
 * This is a two-step conversion via UTF-16LE.
 *
 * @param src     UTF-8 string (must not be NULL).
 * @param src_len Byte length of @p src, or -1 if @p src is null-terminated.
 * @param buf     Output buffer to receive MBCS bytes (must not be NULL).
 * @param size    Size of @p buf in bytes.
 * @return On success, the number of MBCS bytes written (excluding the
 *         null terminator).  If @p buf is too small, the content is
 *         truncated, null-terminated, and the required buffer size is
 *         returned as a positive value.  On error, returns a negative
 *         value.
 */
NEI_API int nei_utf8_to_mbcs(const char *src, int src_len,
                             char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* NEI_CORE_ENCODING_H */
