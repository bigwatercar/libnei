#pragma once
#ifndef NEI_UTILS_SHA1_H
#define NEI_UTILS_SHA1_H

#include <nei/macros/nei_export.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief SHA1 digest length in bytes. */
#define NEI_SHA1_DIGEST_SIZE 20U
/** @brief Lowercase SHA1 hex string length including trailing '\\0'. */
#define NEI_SHA1_HEX_SIZE 41U

/**
 * @brief Incremental SHA1 context.
 * @note Treat fields as internal state; callers should not modify them directly.
 */
typedef struct nei_sha1_ctx_st {
  uint32_t state[5];
  uint64_t total_len;
  uint8_t buffer[64];
  size_t buffer_len;
} nei_sha1_ctx_st;

/**
 * @brief Initialize a SHA1 context for incremental hashing.
 * @param ctx Target context.
 */
NEI_API void nei_sha1_init(nei_sha1_ctx_st *ctx);

/**
 * @brief Feed input bytes into a SHA1 context.
 * @param ctx Target context.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 */
NEI_API void nei_sha1_update(nei_sha1_ctx_st *ctx, const void *data, size_t len);

/**
 * @brief Finalize incremental SHA1 and write binary digest.
 * @param ctx Target context.
 * @param out_digest Output 20-byte digest buffer.
 */
NEI_API void nei_sha1_final(nei_sha1_ctx_st *ctx, uint8_t out_digest[NEI_SHA1_DIGEST_SIZE]);

/**
 * @brief One-shot SHA1 for an in-memory buffer.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 * @param out_digest Output 20-byte digest buffer.
 */
NEI_API void nei_sha1_sum(const void *data, size_t len, uint8_t out_digest[NEI_SHA1_DIGEST_SIZE]);

/**
 * @brief Convert binary SHA1 digest to lowercase hex string.
 * @param digest Input 20-byte digest.
 * @param out_hex Output string buffer of size @ref NEI_SHA1_HEX_SIZE.
 */
NEI_API void nei_sha1_to_hex(const uint8_t digest[NEI_SHA1_DIGEST_SIZE], char out_hex[NEI_SHA1_HEX_SIZE]);

/**
 * @brief One-shot SHA1 and output lowercase hex string.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 * @param out_hex Output string buffer of size @ref NEI_SHA1_HEX_SIZE.
 * @return 0 on success, -1 on invalid arguments.
 */
NEI_API int nei_sha1_sum_hex(const void *data, size_t len, char out_hex[NEI_SHA1_HEX_SIZE]);

/**
 * @brief Compute SHA1 digest for a file.
 * @param file_path Path to file (binary mode).
 * @param out_digest Output 20-byte digest buffer.
 * @return 0 on success, -1 on file I/O error or invalid arguments.
 */
NEI_API int nei_sha1_file_sum(const char *file_path, uint8_t out_digest[NEI_SHA1_DIGEST_SIZE]);

/**
 * @brief Compute SHA1 digest for a file and output lowercase hex string.
 * @param file_path Path to file (binary mode).
 * @param out_hex Output string buffer of size @ref NEI_SHA1_HEX_SIZE.
 * @return 0 on success, -1 on file I/O error or invalid arguments.
 */
NEI_API int nei_sha1_file_sum_hex(const char *file_path, char out_hex[NEI_SHA1_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* NEI_UTILS_SHA1_H */
