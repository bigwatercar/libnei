#pragma once
#ifndef NEI_UTILS_MD5_H
#define NEI_UTILS_MD5_H

#include <nei/macros/nei_export.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief MD5 digest length in bytes. */
#define NEI_MD5_DIGEST_SIZE 16U
/** @brief Lowercase MD5 hex string length including trailing '\\0'. */
#define NEI_MD5_HEX_SIZE 33U

/**
 * @brief Incremental MD5 context.
 * @note Treat fields as internal state; callers should not modify them directly.
 */
typedef struct nei_md5_ctx_st {
  uint32_t state[4];
  uint64_t total_len;
  uint8_t buffer[64];
  size_t buffer_len;
} nei_md5_ctx_st;

/**
 * @brief Initialize an MD5 context for incremental hashing.
 * @param ctx Target context.
 */
NEI_API void nei_md5_init(nei_md5_ctx_st *ctx);

/**
 * @brief Feed input bytes into an MD5 context.
 * @param ctx Target context.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 */
NEI_API void nei_md5_update(nei_md5_ctx_st *ctx, const void *data, size_t len);

/**
 * @brief Finalize incremental MD5 and write binary digest.
 * @param ctx Target context.
 * @param out_digest Output 16-byte digest buffer.
 */
NEI_API void nei_md5_final(nei_md5_ctx_st *ctx, uint8_t out_digest[NEI_MD5_DIGEST_SIZE]);

/**
 * @brief One-shot MD5 for an in-memory buffer.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 * @param out_digest Output 16-byte digest buffer.
 */
NEI_API void nei_md5_sum(const void *data, size_t len, uint8_t out_digest[NEI_MD5_DIGEST_SIZE]);

/**
 * @brief Convert binary MD5 digest to lowercase hex string.
 * @param digest Input 16-byte digest.
 * @param out_hex Output string buffer of size @ref NEI_MD5_HEX_SIZE.
 */
NEI_API void nei_md5_to_hex(const uint8_t digest[NEI_MD5_DIGEST_SIZE], char out_hex[NEI_MD5_HEX_SIZE]);

/**
 * @brief One-shot MD5 and output lowercase hex string.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 * @param out_hex Output string buffer of size @ref NEI_MD5_HEX_SIZE.
 * @return 0 on success, -1 on invalid arguments.
 */
NEI_API int nei_md5_sum_hex(const void *data, size_t len, char out_hex[NEI_MD5_HEX_SIZE]);

/**
 * @brief Compute MD5 digest for a file.
 * @param file_path Path to file (binary mode).
 * @param out_digest Output 16-byte digest buffer.
 * @return 0 on success, -1 on file I/O error or invalid arguments.
 */
NEI_API int nei_md5_file_sum(const char *file_path, uint8_t out_digest[NEI_MD5_DIGEST_SIZE]);

/**
 * @brief Compute MD5 digest for a file and output lowercase hex string.
 * @param file_path Path to file (binary mode).
 * @param out_hex Output string buffer of size @ref NEI_MD5_HEX_SIZE.
 * @return 0 on success, -1 on file I/O error or invalid arguments.
 */
NEI_API int nei_md5_file_sum_hex(const char *file_path, char out_hex[NEI_MD5_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* NEI_UTILS_MD5_H */
