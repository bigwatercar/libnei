#include <nei/utils/uuid.h>
#include <nei/core/random.h>

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
