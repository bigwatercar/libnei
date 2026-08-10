#pragma once
#ifndef NEI_UTILS_CRC32_H
#define NEI_UTILS_CRC32_H

#include <stddef.h>
#include <stdint.h>

#include <nei/build/nei_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief CRC32 digest length in bytes. */
#define NEI_CRC32_DIGEST_SIZE 4U
/** @brief Lowercase CRC32 hex string length including trailing '\0'. */
#define NEI_CRC32_HEX_SIZE 9U

/**
 * @brief Incremental CRC32 context.
 * @note Treat fields as internal state; callers should not modify them directly.
 */
typedef struct nei_crc32_ctx_st {
  uint32_t state;
} nei_crc32_ctx_st;

/**
 * @brief Initialize a CRC32 context for incremental checksumming.
 * @param ctx Target context.
 */
NEI_API void nei_crc32_init(nei_crc32_ctx_st *ctx);

/**
 * @brief Feed input bytes into a CRC32 context.
 * @param ctx Target context.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 */
NEI_API void nei_crc32_update(nei_crc32_ctx_st *ctx, const void *data, size_t len);

/**
 * @brief Finalize incremental CRC32 and return the checksum value.
 * @param ctx Target context.
 * @return Final CRC32 value, or 0 if @p ctx is NULL.
 */
NEI_API uint32_t nei_crc32_final(nei_crc32_ctx_st *ctx);

/**
 * @brief One-shot CRC32 for an in-memory buffer.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 * @return CRC32 checksum value, or 0 if arguments are invalid.
 */
NEI_API uint32_t nei_crc32_sum(const void *data, size_t len);

/**
 * @brief Convert a CRC32 value to lowercase hex string.
 * @param checksum Input CRC32 checksum value.
 * @param out_hex Output string buffer of size @ref NEI_CRC32_HEX_SIZE.
 */
NEI_API void nei_crc32_to_hex(uint32_t checksum, char out_hex[NEI_CRC32_HEX_SIZE]);

/**
 * @brief One-shot CRC32 and output lowercase hex string.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 * @param out_hex Output string buffer of size @ref NEI_CRC32_HEX_SIZE.
 * @return 0 on success, -1 on invalid arguments.
 */
NEI_API int nei_crc32_sum_hex(const void *data, size_t len, char out_hex[NEI_CRC32_HEX_SIZE]);

/**
 * @brief Compute CRC32 for a file.
 * @param file_path Path to file (binary mode).
 * @param out_checksum Output CRC32 checksum value.
 * @return 0 on success, -1 on file I/O error or invalid arguments.
 */
NEI_API int nei_crc32_file_sum(const char *file_path, uint32_t *out_checksum);

/**
 * @brief Compute CRC32 for a file and output lowercase hex string.
 * @param file_path Path to file (binary mode).
 * @param out_hex Output string buffer of size @ref NEI_CRC32_HEX_SIZE.
 * @return 0 on success, -1 on file I/O error or invalid arguments.
 */
NEI_API int nei_crc32_file_sum_hex(const char *file_path, char out_hex[NEI_CRC32_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* NEI_UTILS_CRC32_H */