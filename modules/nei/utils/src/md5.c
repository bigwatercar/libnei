#include <nei/utils/md5.h>

#include <stdio.h>
#include <string.h>

#define NEI_MD5_F(x, y, z) (((x) & (y)) | ((~(x)) & (z)))
#define NEI_MD5_G(x, y, z) (((x) & (z)) | ((y) & (~(z))))
#define NEI_MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define NEI_MD5_I(x, y, z) ((y) ^ ((x) | (~(z))))
#define NEI_MD5_ROTL32(x, n) (((x) << (n)) | ((x) >> (32U - (n))))

#define NEI_MD5_STEP(func, a, b, c, d, xk, t, s) \
  do {                                              \
    (a) += func((b), (c), (d)) + (xk) + (t);       \
    (a) = (b) + NEI_MD5_ROTL32((a), (s));          \
  } while (0)

static uint32_t nei_md5_load_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void nei_md5_store_u32_le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
  p[2] = (uint8_t)((v >> 16U) & 0xFFU);
  p[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static void nei_md5_transform(uint32_t state[4], const uint8_t block[64]) {
  static const uint32_t s_table[64] = {
      7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U,
      5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U, 5U, 9U, 14U, 20U,
      4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U,
      6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U,
  };
  static const uint32_t k_table[64] = {
      0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
      0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
      0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
      0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
      0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
      0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
      0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
      0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U,
  };

  uint32_t x[16];
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t i;

  for (i = 0U; i < 16U; ++i) {
    x[i] = nei_md5_load_u32_le(block + i * 4U);
  }

  for (i = 0U; i < 64U; ++i) {
    uint32_t g;
    if (i < 16U) {
      g = i;
      NEI_MD5_STEP(NEI_MD5_F, a, b, c, d, x[g], k_table[i], s_table[i]);
    } else if (i < 32U) {
      g = (5U * i + 1U) & 0x0FU;
      NEI_MD5_STEP(NEI_MD5_G, a, b, c, d, x[g], k_table[i], s_table[i]);
    } else if (i < 48U) {
      g = (3U * i + 5U) & 0x0FU;
      NEI_MD5_STEP(NEI_MD5_H, a, b, c, d, x[g], k_table[i], s_table[i]);
    } else {
      g = (7U * i) & 0x0FU;
      NEI_MD5_STEP(NEI_MD5_I, a, b, c, d, x[g], k_table[i], s_table[i]);
    }

    {
      uint32_t tmp = d;
      d = c;
      c = b;
      b = a;
      a = tmp;
    }
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

void nei_md5_init(nei_md5_ctx_st *ctx) {
  if (ctx == NULL) {
    return;
  }
  ctx->state[0] = 0x67452301U;
  ctx->state[1] = 0xefcdab89U;
  ctx->state[2] = 0x98badcfeU;
  ctx->state[3] = 0x10325476U;
  ctx->total_len = 0ULL;
  ctx->buffer_len = 0U;
  memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void nei_md5_update(nei_md5_ctx_st *ctx, const void *data, size_t len) {
  const uint8_t *in = (const uint8_t *)data;
  size_t offset = 0U;

  if (ctx == NULL || (data == NULL && len > 0U)) {
    return;
  }

  ctx->total_len += (uint64_t)len;

  if (ctx->buffer_len > 0U) {
    const size_t need = 64U - ctx->buffer_len;
    const size_t take = (len < need) ? len : need;
    memcpy(ctx->buffer + ctx->buffer_len, in, take);
    ctx->buffer_len += take;
    offset += take;
    if (ctx->buffer_len == 64U) {
      nei_md5_transform(ctx->state, ctx->buffer);
      ctx->buffer_len = 0U;
    }
  }

  while (offset + 64U <= len) {
    nei_md5_transform(ctx->state, in + offset);
    offset += 64U;
  }

  if (offset < len) {
    const size_t remain = len - offset;
    memcpy(ctx->buffer, in + offset, remain);
    ctx->buffer_len = remain;
  }
}

void nei_md5_final(nei_md5_ctx_st *ctx, uint8_t out_digest[NEI_MD5_DIGEST_SIZE]) {
  uint8_t pad[64];
  uint8_t len_bytes[8];
  uint64_t bit_len;
  size_t i;

  if (ctx == NULL || out_digest == NULL) {
    return;
  }

  memset(pad, 0, sizeof(pad));
  pad[0] = 0x80U;

  bit_len = ctx->total_len * 8ULL;
  for (i = 0U; i < 8U; ++i) {
    len_bytes[i] = (uint8_t)((bit_len >> (8U * i)) & 0xFFU);
  }

  if (ctx->buffer_len < 56U) {
    nei_md5_update(ctx, pad, 56U - ctx->buffer_len);
  } else {
    nei_md5_update(ctx, pad, 64U - ctx->buffer_len);
    nei_md5_update(ctx, pad + 1U, 56U);
  }
  nei_md5_update(ctx, len_bytes, 8U);

  for (i = 0U; i < 4U; ++i) {
    nei_md5_store_u32_le(out_digest + i * 4U, ctx->state[i]);
  }
}

void nei_md5_sum(const void *data, size_t len, uint8_t out_digest[NEI_MD5_DIGEST_SIZE]) {
  nei_md5_ctx_st ctx;
  nei_md5_init(&ctx);
  nei_md5_update(&ctx, data, len);
  nei_md5_final(&ctx, out_digest);
}

void nei_md5_to_hex(const uint8_t digest[NEI_MD5_DIGEST_SIZE], char out_hex[NEI_MD5_HEX_SIZE]) {
  static const char hex[] = "0123456789abcdef";
  size_t i;

  if (digest == NULL || out_hex == NULL) {
    return;
  }
  for (i = 0U; i < NEI_MD5_DIGEST_SIZE; ++i) {
    out_hex[i * 2U] = hex[(digest[i] >> 4U) & 0x0FU];
    out_hex[i * 2U + 1U] = hex[digest[i] & 0x0FU];
  }
  out_hex[NEI_MD5_HEX_SIZE - 1U] = '\0';
}

int nei_md5_sum_hex(const void *data, size_t len, char out_hex[NEI_MD5_HEX_SIZE]) {
  uint8_t digest[NEI_MD5_DIGEST_SIZE];
  if (out_hex == NULL || (data == NULL && len > 0U)) {
    return -1;
  }
  nei_md5_sum(data, len, digest);
  nei_md5_to_hex(digest, out_hex);
  return 0;
}

int nei_md5_file_sum(const char *file_path, uint8_t out_digest[NEI_MD5_DIGEST_SIZE]) {
  FILE *fp;
  uint8_t buf[4096];
  nei_md5_ctx_st ctx;
  size_t n;

  if (file_path == NULL || out_digest == NULL) {
    return -1;
  }

#if defined(_WIN32)
  {
    errno_t err;
    err = fopen_s(&fp, file_path, "rb");
    if (err != 0) {
      return -1;
    }
  }
#else
  fp = fopen(file_path, "rb");
  if (fp == NULL) {
    return -1;
  }
#endif

  nei_md5_init(&ctx);
  for (;;) {
    n = fread(buf, 1U, sizeof(buf), fp);
    if (n > 0U) {
      nei_md5_update(&ctx, buf, n);
    }
    if (n < sizeof(buf)) {
      if (ferror(fp) != 0) {
        fclose(fp);
        return -1;
      }
      break;
    }
  }

  fclose(fp);
  nei_md5_final(&ctx, out_digest);
  return 0;
}

int nei_md5_file_sum_hex(const char *file_path, char out_hex[NEI_MD5_HEX_SIZE]) {
  uint8_t digest[NEI_MD5_DIGEST_SIZE];
  if (out_hex == NULL) {
    return -1;
  }
  if (nei_md5_file_sum(file_path, digest) != 0) {
    return -1;
  }
  nei_md5_to_hex(digest, out_hex);
  return 0;
}
