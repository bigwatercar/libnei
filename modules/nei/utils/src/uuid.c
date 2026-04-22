#include <nei/utils/uuid.h>

#include <stddef.h>
#include <time.h>

#if defined(_WIN32)
#include <Windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static int nei_uuid_fill_random(uint8_t *out, size_t len) {
#if defined(_WIN32)
  if (BCryptGenRandom(NULL, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
    return NEI_UUID_OK_STRONG;
  }
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    size_t done = 0U;
    while (done < len) {
      const ssize_t n = read(fd, out + done, len - done);
      if (n <= 0) {
        close(fd);
        fd = -1;
        break;
      }
      done += (size_t)n;
    }
    if (fd >= 0) {
      close(fd);
      return NEI_UUID_OK_STRONG;
    }
  }
#endif

  {
    /* Fallback when OS entropy is unavailable. */
    static uint64_t s_degraded_counter = 0ULL;
    const uint64_t counter = ++s_degraded_counter;
    uint64_t x = (uint64_t)time(NULL) ^ (uint64_t)clock() ^ (uintptr_t)out ^ ((uint64_t)len << 32U) ^ counter;
    size_t i;
    for (i = 0U; i < len; ++i) {
      x ^= x >> 12U;
      x ^= x << 25U;
      x ^= x >> 27U;
      x *= 2685821657736338717ULL;
      out[i] = (uint8_t)(x & 0xFFU);
    }
  }

  return NEI_UUID_OK_DEGRADED;
}

int nei_uuid4_generate(uint8_t out_uuid[NEI_UUID_BINARY_SIZE]) {
  int rc;
  if (out_uuid == NULL) {
    return NEI_UUID_ERR_INVALID_ARG;
  }
  rc = nei_uuid_fill_random(out_uuid, NEI_UUID_BINARY_SIZE);

  out_uuid[6] = (uint8_t)((out_uuid[6] & 0x0FU) | 0x40U); /* version 4 */
  out_uuid[8] = (uint8_t)((out_uuid[8] & 0x3FU) | 0x80U); /* RFC 4122 variant */
  return rc;
}

int nei_uuid_to_string(const uint8_t uuid[NEI_UUID_BINARY_SIZE], char out_str[NEI_UUID_STRING_SIZE]) {
  static const char hex[] = "0123456789abcdef";
  static const uint8_t dash_before[] = {4U, 6U, 8U, 10U};
  size_t i;
  size_t p = 0U;
  size_t d = 0U;

  if (uuid == NULL || out_str == NULL) {
    return -1;
  }

  for (i = 0U; i < NEI_UUID_BINARY_SIZE; ++i) {
    if (d < sizeof(dash_before) / sizeof(dash_before[0]) && i == dash_before[d]) {
      out_str[p++] = '-';
      ++d;
    }
    out_str[p++] = hex[(uuid[i] >> 4U) & 0x0FU];
    out_str[p++] = hex[uuid[i] & 0x0FU];
  }
  out_str[p] = '\0';
  return 0;
}

int nei_uuid4_generate_string(char out_str[NEI_UUID_STRING_SIZE]) {
  uint8_t uuid[NEI_UUID_BINARY_SIZE];
  int rc;
  if (out_str == NULL) {
    return NEI_UUID_ERR_INVALID_ARG;
  }
  rc = nei_uuid4_generate(uuid);
  if (rc < 0) {
    return rc;
  }
  if (nei_uuid_to_string(uuid, out_str) != 0) {
    return NEI_UUID_ERR_INVALID_ARG;
  }
  return rc;
}
