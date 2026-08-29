#include <nei/utils/base64.h>

static int nei_base64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') {
    return (int)(c - 'A');
  }
  if (c >= 'a' && c <= 'z') {
    return (int)(c - 'a') + 26;
  }
  if (c >= '0' && c <= '9') {
    return (int)(c - '0') + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

size_t nei_base64_encoded_length(size_t input_len) {
  return ((input_len + 2U) / 3U) * 4U;
}

size_t nei_base64_decoded_max_length(size_t input_len) {
  return (input_len / 4U) * 3U;
}

int nei_base64_encode(const uint8_t *input, size_t input_len, char *out, size_t out_cap, size_t *out_len) {
  static const char s_enc_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0U;
  size_t o = 0U;
  size_t need = nei_base64_encoded_length(input_len);

  if ((input == NULL && input_len > 0U) || (out == NULL && need > 0U)) {
    return NEI_BASE64_ERR_INVALID_ARG;
  }
  if (out_cap < need) {
    return NEI_BASE64_ERR_OUTPUT_TOO_SMALL;
  }

  while (i + 3U <= input_len) {
    const uint32_t v = ((uint32_t)input[i] << 16U) | ((uint32_t)input[i + 1U] << 8U) | (uint32_t)input[i + 2U];
    out[o++] = s_enc_table[(v >> 18U) & 0x3FU];
    out[o++] = s_enc_table[(v >> 12U) & 0x3FU];
    out[o++] = s_enc_table[(v >> 6U) & 0x3FU];
    out[o++] = s_enc_table[v & 0x3FU];
    i += 3U;
  }

  if (i < input_len) {
    const uint32_t a = input[i];
    const uint32_t b = (i + 1U < input_len) ? input[i + 1U] : 0U;
    const uint32_t v = (a << 16U) | (b << 8U);
    out[o++] = s_enc_table[(v >> 18U) & 0x3FU];
    out[o++] = s_enc_table[(v >> 12U) & 0x3FU];
    if (i + 1U < input_len) {
      out[o++] = s_enc_table[(v >> 6U) & 0x3FU];
      out[o++] = '=';
    } else {
      out[o++] = '=';
      out[o++] = '=';
    }
  }

  if (out_len != NULL) {
    *out_len = o;
  }
  return NEI_BASE64_OK;
}

int nei_base64_decode(const char *input, size_t input_len, uint8_t *out, size_t out_cap, size_t *out_len) {
  size_t i = 0U;
  size_t o = 0U;
  size_t pad = 0U;
  size_t need;

  if ((input == NULL && input_len > 0U) || (out == NULL && input_len > 0U)) {
    return NEI_BASE64_ERR_INVALID_ARG;
  }
  if ((input_len % 4U) != 0U) {
    return NEI_BASE64_ERR_INVALID_INPUT;
  }

  if (input_len > 0U && input[input_len - 1U] == '=') {
    pad = 1U;
    if (input_len > 1U && input[input_len - 2U] == '=') {
      pad = 2U;
    }
  }
  need = nei_base64_decoded_max_length(input_len) - pad;
  if (out_cap < need) {
    return NEI_BASE64_ERR_OUTPUT_TOO_SMALL;
  }

  while (i < input_len) {
    const unsigned char c0 = (unsigned char)input[i];
    const unsigned char c1 = (unsigned char)input[i + 1U];
    const unsigned char c2 = (unsigned char)input[i + 2U];
    const unsigned char c3 = (unsigned char)input[i + 3U];
    const int v0 = nei_base64_value(c0);
    const int v1 = nei_base64_value(c1);
    const int is_last = (i + 4U == input_len) ? 1 : 0;

    if (v0 < 0 || v1 < 0) {
      return NEI_BASE64_ERR_INVALID_INPUT;
    }

    if (c2 == '=') {
      if (!is_last || c3 != '=') {
        return NEI_BASE64_ERR_INVALID_INPUT;
      }
      out[o++] = (uint8_t)(((uint32_t)v0 << 2U) | ((uint32_t)v1 >> 4U));
    } else {
      const int v2 = nei_base64_value(c2);
      if (v2 < 0) {
        return NEI_BASE64_ERR_INVALID_INPUT;
      }
      if (c3 == '=') {
        if (!is_last) {
          return NEI_BASE64_ERR_INVALID_INPUT;
        }
        out[o++] = (uint8_t)(((uint32_t)v0 << 2U) | ((uint32_t)v1 >> 4U));
        out[o++] = (uint8_t)((((uint32_t)v1 & 0x0FU) << 4U) | ((uint32_t)v2 >> 2U));
      } else {
        const int v3 = nei_base64_value(c3);
        if (v3 < 0) {
          return NEI_BASE64_ERR_INVALID_INPUT;
        }
        out[o++] = (uint8_t)(((uint32_t)v0 << 2U) | ((uint32_t)v1 >> 4U));
        out[o++] = (uint8_t)((((uint32_t)v1 & 0x0FU) << 4U) | ((uint32_t)v2 >> 2U));
        out[o++] = (uint8_t)((((uint32_t)v2 & 0x03U) << 6U) | (uint32_t)v3);
      }
    }

    i += 4U;
  }

  if (out_len != NULL) {
    *out_len = o;
  }
  return NEI_BASE64_OK;
}
