#pragma once
#ifndef NEI_UTILS_SHA256_H
#define NEI_UTILS_SHA256_H

#include <stddef.h>
#include <stdint.h>

#include <nei/build/nei_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief SHA256 digest length in bytes. */
#define NEI_SHA256_DIGEST_SIZE 32U
/** @brief Lowercase SHA256 hex string length including trailing '\0'. */
#define NEI_SHA256_HEX_SIZE 65U

/**
 * @brief Incremental SHA256 context.
 * @note Treat fields as internal state; callers should not modify them directly.
 */
typedef struct nei_sha256_ctx_st {
  uint32_t state[8];
  uint64_t total_len;
  uint8_t buffer[64];
  size_t buffer_len;
} nei_sha256_ctx_st;

/**
 * @brief Initialize a SHA256 context for incremental hashing.
 * @param ctx Target context.
 */
NEI_API void nei_sha256_init(nei_sha256_ctx_st *ctx);

/**
 * @brief Feed input bytes into a SHA256 context.
 * @param ctx Target context.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 */
NEI_API void nei_sha256_update(nei_sha256_ctx_st *ctx, const void *data, size_t len);

/**
 * @brief Finalize incremental SHA256 and write binary digest.
 * @param ctx Target context.
 * @param out_digest Output 32-byte digest buffer.
 */
NEI_API void nei_sha256_final(nei_sha256_ctx_st *ctx, uint8_t out_digest[NEI_SHA256_DIGEST_SIZE]);

/**
 * @brief One-shot SHA256 for an in-memory buffer.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 * @param out_digest Output 32-byte digest buffer.
 */
NEI_API void nei_sha256_sum(const void *data, size_t len, uint8_t out_digest[NEI_SHA256_DIGEST_SIZE]);

/**
 * @brief Convert binary SHA256 digest to lowercase hex string.
 * @param digest Input 32-byte digest.
 * @param out_hex Output string buffer of size @ref NEI_SHA256_HEX_SIZE.
 */
NEI_API void nei_sha256_to_hex(const uint8_t digest[NEI_SHA256_DIGEST_SIZE], char out_hex[NEI_SHA256_HEX_SIZE]);

/**
 * @brief One-shot SHA256 and output lowercase hex string.
 * @param data Input data pointer.
 * @param len Input size in bytes.
 * @param out_hex Output string buffer of size @ref NEI_SHA256_HEX_SIZE.
 * @return 0 on success, -1 on invalid arguments.
 */
NEI_API int nei_sha256_sum_hex(const void *data, size_t len, char out_hex[NEI_SHA256_HEX_SIZE]);

/**
 * @brief Compute SHA256 digest for a file.
 * @param file_path Path to file (binary mode).
 * @param out_digest Output 32-byte digest buffer.
 * @return 0 on success, -1 on file I/O error or invalid arguments.
 */
NEI_API int nei_sha256_file_sum(const char *file_path, uint8_t out_digest[NEI_SHA256_DIGEST_SIZE]);

/**
 * @brief Compute SHA256 digest for a file and output lowercase hex string.
 * @param file_path Path to file (binary mode).
 * @param out_hex Output string buffer of size @ref NEI_SHA256_HEX_SIZE.
 * @return 0 on success, -1 on file I/O error or invalid arguments.
 */
NEI_API int nei_sha256_file_sum_hex(const char *file_path, char out_hex[NEI_SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* NEI_UTILS_SHA256_H */