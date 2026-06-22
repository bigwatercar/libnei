#include <nei/utils/random.h>

#include <stddef.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <Windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal: platform-aware cryptographic random byte filler.
 * Returns NEI_RANDOM_OK on strong entropy, NEI_RANDOM_OK_DEGRADED on
 * fallback PRNG.
 * --------------------------------------------------------------------------- */
static int nei_random_fill_bytes(void *out, size_t len) {
#if defined(_WIN32)
  if (BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)len,
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
    return NEI_RANDOM_OK;
  }
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    size_t done = 0U;
    while (done < len) {
      const ssize_t n = read(fd, (unsigned char *)out + done, len - done);
      if (n <= 0) {
        close(fd);
        fd = -1;
        break;
      }
      done += (size_t)n;
    }
    if (fd >= 0) {
      close(fd);
      return NEI_RANDOM_OK;
    }
  }
#endif

  /* Fallback: xorshift64* PRNG seeded with time + clock + pointer + counter. */
  {
    static uint64_t s_degraded_counter = 0ULL;
    const uint64_t counter = ++s_degraded_counter;
    uint64_t x = (uint64_t)time(NULL) ^ (uint64_t)clock() ^ (uintptr_t)out ^
                 ((uint64_t)len << 32U) ^ counter;
    unsigned char *dst = (unsigned char *)out;
    size_t i;
    for (i = 0U; i < len; ++i) {
      x ^= x >> 12U;
      x ^= x << 25U;
      x ^= x >> 27U;
      x *= 2685821657736338717ULL;
      dst[i] = (unsigned char)(x & 0xFFU);
    }
  }

  return NEI_RANDOM_OK_DEGRADED;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

int nei_random_string(char *out, size_t len, const char *charset) {
  size_t charset_len;
  size_t i;

  if (out == NULL || len == 0U) {
    return NEI_RANDOM_ERR_INVALID_ARG;
  }

  if (charset == NULL) {
    charset = NEI_RANDOM_DEFAULT_CHARSET;
  }

  charset_len = strlen(charset);
  if (charset_len == 0U) {
    return NEI_RANDOM_ERR_INVALID_ARG;
  }

  /* Generate raw random bytes and map each to a character from the charset. */
  {
    unsigned char *raw = (unsigned char *)out;
    int rc = nei_random_fill_bytes(raw, len);
    for (i = 0U; i < len; ++i) {
      out[i] = charset[raw[i] % charset_len];
    }
    out[len] = '\0';
    return rc;
  }
}

int nei_random_buffer(void *out, size_t len) {
  if (out == NULL || len == 0U) {
    return NEI_RANDOM_ERR_INVALID_ARG;
  }
  return nei_random_fill_bytes(out, len);
}
