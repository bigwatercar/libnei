#pragma once
#ifndef NEI_UTILS_BASE64_H
#define NEI_UTILS_BASE64_H

#include <nei/macros/nei_export.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Success. */
#define NEI_BASE64_OK 0
/** @brief Error: invalid argument. */
#define NEI_BASE64_ERR_INVALID_ARG -1
/** @brief Error: output buffer is too small. */
#define NEI_BASE64_ERR_OUTPUT_TOO_SMALL -2
/** @brief Error: input is not valid RFC 4648 base64 text. */
#define NEI_BASE64_ERR_INVALID_INPUT -3

/**
 * @brief Returns encoded base64 text length (without trailing '\0').
 * @param input_len Input byte length.
 * @return Required base64 character count.
 */
NEI_API size_t nei_base64_encoded_length(size_t input_len);

/**
 * @brief Returns maximum possible decoded byte length for given base64 text length.
 * @param input_len Base64 text length.
 * @return Maximum decoded byte count.
 */
NEI_API size_t nei_base64_decoded_max_length(size_t input_len);

/**
 * @brief Encode bytes to RFC 4648 base64 text.
 * @param input Input bytes.
 * @param input_len Input length.
 * @param out Output buffer for base64 text.
 * @param out_cap Output buffer capacity in bytes.
 * @param out_len Optional output length (without trailing '\0').
 * @return @ref NEI_BASE64_OK on success, otherwise an error code.
 */
NEI_API int nei_base64_encode(
    const uint8_t *input, size_t input_len, char *out, size_t out_cap, size_t *out_len);

/**
 * @brief Decode RFC 4648 base64 text into raw bytes.
 * @param input Base64 text bytes (no implicit '\0' required).
 * @param input_len Base64 text length.
 * @param out Output byte buffer.
 * @param out_cap Output buffer capacity.
 * @param out_len Optional decoded byte length.
 * @return @ref NEI_BASE64_OK on success, otherwise an error code.
 */
NEI_API int nei_base64_decode(
    const char *input, size_t input_len, uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* NEI_UTILS_BASE64_H */
