#pragma once
#ifndef NEI_UTILS_UUID_H
#define NEI_UTILS_UUID_H

#include <nei/macros/nei_export.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief UUID binary length in bytes. */
#define NEI_UUID_BINARY_SIZE 16U
/** @brief Canonical UUID string length including trailing '\\0'. */
#define NEI_UUID_STRING_SIZE 37U

/** @brief Success: UUID generated from system entropy source. */
#define NEI_UUID_OK_STRONG 0
/** @brief Success: UUID generated from degraded fallback RNG. */
#define NEI_UUID_OK_DEGRADED 1
/** @brief Error: invalid argument. */
#define NEI_UUID_ERR_INVALID_ARG -1

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

#ifdef __cplusplus
}
#endif

#endif /* NEI_UTILS_UUID_H */
