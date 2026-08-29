#pragma once
#ifndef NEI_UTILS_UUID_H
#define NEI_UTILS_UUID_H

#include <stdint.h>

#include <nei/build/nei_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief UUID binary length in bytes. */
#define NEI_UUID_BINARY_SIZE 16U
/** @brief Canonical UUID string length including trailing '\\0'. */
#define NEI_UUID_STRING_SIZE 37U
/** @brief Braced UUID string length (wrapped in '{}') including trailing '\\0'. */
#define NEI_UUID_BRACED_STRING_SIZE 39U

/** @brief Success: UUID generated from system entropy source. */
#define NEI_UUID_OK_STRONG 0
/** @brief Success: UUID generated from degraded fallback RNG. */
#define NEI_UUID_OK_DEGRADED 1
/** @brief Error: invalid argument. */
#define NEI_UUID_ERR_INVALID_ARG -1
/** @brief Error: string is not a valid UUID. */
#define NEI_UUID_ERR_INVALID_FORMAT -2

/* Generate an RFC 4122 version 4 UUID in binary form.
 * Returns NEI_UUID_OK_STRONG or NEI_UUID_OK_DEGRADED on success.
 */
NEI_API int nei_uuid4_generate(uint8_t out_uuid[NEI_UUID_BINARY_SIZE]);

/**
 * @brief Convert binary UUID to lowercase canonical string.
 * @param uuid Input 16-byte UUID.
 * @param out_str Output string buffer of size @ref NEI_UUID_STRING_SIZE.
 * @return 0 on success, -1 on invalid arguments.
 * @details Output format is xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.
 */
NEI_API int nei_uuid_to_string(const uint8_t uuid[NEI_UUID_BINARY_SIZE], char out_str[NEI_UUID_STRING_SIZE]);

/* Generate an RFC 4122 version 4 UUID and format as canonical string.
 * Returns NEI_UUID_OK_STRONG or NEI_UUID_OK_DEGRADED on success.
 */
NEI_API int nei_uuid4_generate_string(char out_str[NEI_UUID_STRING_SIZE]);

/**
 * @brief Parse a UUID string into binary form.
 * @param str Input string; accepts the canonical form
 *            "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" with or without surrounding
 *            braces "{}". Hex digits are case-insensitive.
 * @param out_uuid Output 16-byte UUID.
 * @return 0 on success, NEI_UUID_ERR_INVALID_ARG on null arguments, or
 *         NEI_UUID_ERR_INVALID_FORMAT if @p str is not a valid UUID.
 */
NEI_API int nei_uuid_from_string(const char *str, uint8_t out_uuid[NEI_UUID_BINARY_SIZE]);

/**
 * @brief Compare two UUIDs byte-wise with memcmp semantics.
 * @param a First 16-byte UUID.
 * @param b Second 16-byte UUID.
 * @return <0 if @p a < @p b, 0 if @p a == @p b, >0 if @p a > @p b.
 * @details A NULL UUID is ordered before any non-NULL UUID; two NULL UUIDs are
 *          considered equal.
 */
NEI_API int nei_uuid_compare(const uint8_t a[NEI_UUID_BINARY_SIZE], const uint8_t b[NEI_UUID_BINARY_SIZE]);

/**
 * @brief Check two UUIDs for equality.
 * @param a First 16-byte UUID.
 * @param b Second 16-byte UUID.
 * @return 1 if both are non-NULL and equal, 0 otherwise. A NULL argument is
 *         never equal to anything.
 */
NEI_API int nei_uuid_equal(const uint8_t a[NEI_UUID_BINARY_SIZE], const uint8_t b[NEI_UUID_BINARY_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* NEI_UTILS_UUID_H */
