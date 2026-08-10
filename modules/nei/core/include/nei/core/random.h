#pragma once
#ifndef NEI_CORE_RANDOM_H
#define NEI_CORE_RANDOM_H

#include <stddef.h>
#include <stdint.h>

#include <nei/build/nei_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Success: random data generated from system entropy source. */
#define NEI_RANDOM_OK 0
/** @brief Success: random data generated from degraded fallback RNG. */
#define NEI_RANDOM_OK_DEGRADED 1
/** @brief Error: invalid argument (e.g. NULL output buffer or zero length). */
#define NEI_RANDOM_ERR_INVALID_ARG -1

/**
 * @brief Default character set for random strings.
 * @details Consists of lowercase/uppercase letters, digits, hyphen and
 *          underscore.  These characters are safe for filenames on
 *          Windows, Linux, and macOS.
 */
#define NEI_RANDOM_DEFAULT_CHARSET "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"

/**
 * @brief Generate a random string using characters from a given charset.
 *
 * @param out     Output buffer. Must be at least @p len + 1 bytes to
 *                accommodate the null terminator.
 * @param len     Number of random characters to generate (excluding the
 *                null terminator).
 * @param charset Candidate character set as a null-terminated string.
 *                If NULL, @ref NEI_RANDOM_DEFAULT_CHARSET is used.
 * @return @ref NEI_RANDOM_OK on success (strong entropy),
 *         @ref NEI_RANDOM_OK_DEGRADED on success (degraded fallback),
 *         @ref NEI_RANDOM_ERR_INVALID_ARG on invalid arguments.
 *
 * @note The output is always null-terminated.  The caller must ensure
 *       the output buffer has room for @p len characters plus one null
 *       byte.
 */
NEI_API int nei_random_string(char *out, size_t len, const char *charset);

/**
 * @brief Fill a buffer with cryptographically random bytes.
 *
 * @param out Output buffer to fill.
 * @param len Number of random bytes to write.
 * @return @ref NEI_RANDOM_OK on success (system entropy),
 *         @ref NEI_RANDOM_OK_DEGRADED on success (degraded fallback),
 *         @ref NEI_RANDOM_ERR_INVALID_ARG on invalid arguments.
 *
 * @note On most platforms this uses OS-provided cryptographic RNG
 *       (e.g. `BCryptGenRandom` on Windows, `/dev/urandom` on POSIX).
 *       If the OS entropy source is unavailable, a fallback PRNG is
 *       used.
 */
NEI_API int nei_random_buffer(void *out, size_t len);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------------------------------------------
 * C++ convenience wrappers (inline)
 * --------------------------------------------------------------------------- */
#ifdef __cplusplus

#include <string>
#include <vector>

inline std::string nei_random_string(size_t len, const std::string &charset = {}) {
  std::string result(len, '\0');
  nei_random_string(&result[0], len, charset.empty() ? nullptr : charset.c_str());
  return result;
}

inline std::vector<uint8_t> nei_random_buffer(size_t len) {
  std::vector<uint8_t> result(len);
  nei_random_buffer(result.data(), len);
  return result;
}

#endif /* __cplusplus */
#endif /* NEI_CORE_RANDOM_H */
