#include <nei/utils/uuid.h>

#include <string.h>

#include <nei/core/random.h>

static int HexNibble(char c) {
  if (c >= '0' && c <= '9') {
    return (int)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (int)(c - 'a') + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return (int)(c - 'A') + 10;
  }
  return -1;
}

int nei_uuid4_generate(uint8_t out_uuid[NEI_UUID_BINARY_SIZE]) {
  int rc;
  if (out_uuid == NULL) {
    return NEI_UUID_ERR_INVALID_ARG;
  }
  rc = nei_random_buffer(out_uuid, NEI_UUID_BINARY_SIZE);

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

int nei_uuid_from_string(const char *str, uint8_t out_uuid[NEI_UUID_BINARY_SIZE]) {
  static const size_t dash_at[] = {8U, 13U, 18U, 23U};
  size_t len;
  size_t i;
  size_t nibbles = 0U;
  int has_braces;

  if (str == NULL || out_uuid == NULL) {
    return NEI_UUID_ERR_INVALID_ARG;
  }

  len = strlen(str);
  if (len == 0U) {
    return NEI_UUID_ERR_INVALID_FORMAT;
  }

  has_braces = (str[0] == '{') && (str[len - 1U] == '}');
  if (has_braces) {
    if (len != NEI_UUID_BRACED_STRING_SIZE - 1U) {
      return NEI_UUID_ERR_INVALID_FORMAT;
    }
    ++str;
    len -= 2U;
  } else if (len != NEI_UUID_STRING_SIZE - 1U) {
    return NEI_UUID_ERR_INVALID_FORMAT;
  }

  for (i = 0U; i < len; ++i) {
    char c;
    int nibble;
    c = str[i];
    if (i == dash_at[0] || i == dash_at[1] || i == dash_at[2] || i == dash_at[3]) {
      if (c != '-') {
        return NEI_UUID_ERR_INVALID_FORMAT;
      }
      continue;
    }
    nibble = HexNibble(c);
    if (nibble < 0) {
      return NEI_UUID_ERR_INVALID_FORMAT;
    }
    if ((nibbles & 1U) == 0U) {
      out_uuid[nibbles >> 1U] = (uint8_t)(nibble << 4);
    } else {
      out_uuid[nibbles >> 1U] |= (uint8_t)nibble;
    }
    ++nibbles;
  }
  return 0;
}

int nei_uuid_compare(const uint8_t a[NEI_UUID_BINARY_SIZE], const uint8_t b[NEI_UUID_BINARY_SIZE]) {
  if (a == NULL && b == NULL) {
    return 0;
  }
  if (a == NULL) {
    return -1;
  }
  if (b == NULL) {
    return 1;
  }
  return memcmp(a, b, NEI_UUID_BINARY_SIZE);
}

int nei_uuid_equal(const uint8_t a[NEI_UUID_BINARY_SIZE], const uint8_t b[NEI_UUID_BINARY_SIZE]) {
  return (a != NULL) && (b != NULL) && memcmp(a, b, NEI_UUID_BINARY_SIZE) == 0;
}
