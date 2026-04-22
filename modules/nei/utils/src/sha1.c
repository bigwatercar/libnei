#include <nei/utils/sha1.h>

#include <stdio.h>
#include <string.h>

#define NEI_SHA1_ROTL32(x, n) (((x) << (n)) | ((x) >> (32U - (n))))

static uint32_t nei_sha1_load_u32_be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) | ((uint32_t)p[2] << 8U) | (uint32_t)p[3];
}

static void nei_sha1_store_u32_be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)((v >> 24U) & 0xFFU);
  p[1] = (uint8_t)((v >> 16U) & 0xFFU);
  p[2] = (uint8_t)((v >> 8U) & 0xFFU);
  p[3] = (uint8_t)(v & 0xFFU);
}

static void nei_sha1_transform(uint32_t state[5], const uint8_t block[64]) {
  uint32_t w[80];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t i;

  for (i = 0U; i < 16U; ++i) {
    w[i] = nei_sha1_load_u32_be(block + i * 4U);
  }
  for (i = 16U; i < 80U; ++i) {
    w[i] = NEI_SHA1_ROTL32(w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U], 1U);
  }

  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];

  for (i = 0U; i < 80U; ++i) {
    uint32_t f;
    uint32_t k;
    uint32_t temp;

    if (i < 20U) {
      f = (b & c) | ((~b) & d);
      k = 0x5a827999U;
    } else if (i < 40U) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1U;
    } else if (i < 60U) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdcU;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6U;
    }

    temp = NEI_SHA1_ROTL32(a, 5U) + f + e + k + w[i];
    e = d;
    d = c;
    c = NEI_SHA1_ROTL32(b, 30U);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void nei_sha1_init(nei_sha1_ctx_st *ctx) {
  if (ctx == NULL) {
    return;
  }
  ctx->state[0] = 0x67452301U;
  ctx->state[1] = 0xefcdab89U;
  ctx->state[2] = 0x98badcfeU;
  ctx->state[3] = 0x10325476U;
  ctx->state[4] = 0xc3d2e1f0U;
  ctx->total_len = 0ULL;
  ctx->buffer_len = 0U;
  memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void nei_sha1_update(nei_sha1_ctx_st *ctx, const void *data, size_t len) {
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
      nei_sha1_transform(ctx->state, ctx->buffer);
      ctx->buffer_len = 0U;
    }
  }

  while (offset + 64U <= len) {
    nei_sha1_transform(ctx->state, in + offset);
    offset += 64U;
  }

  if (offset < len) {
    const size_t remain = len - offset;
    memcpy(ctx->buffer, in + offset, remain);
    ctx->buffer_len = remain;
  }
}

void nei_sha1_final(nei_sha1_ctx_st *ctx, uint8_t out_digest[NEI_SHA1_DIGEST_SIZE]) {
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
    len_bytes[7U - i] = (uint8_t)((bit_len >> (8U * i)) & 0xFFU);
  }

  if (ctx->buffer_len < 56U) {
    nei_sha1_update(ctx, pad, 56U - ctx->buffer_len);
  } else {
    nei_sha1_update(ctx, pad, 64U - ctx->buffer_len);
    nei_sha1_update(ctx, pad + 1U, 56U);
  }
  nei_sha1_update(ctx, len_bytes, 8U);

  for (i = 0U; i < 5U; ++i) {
    nei_sha1_store_u32_be(out_digest + i * 4U, ctx->state[i]);
  }
}

void nei_sha1_sum(const void *data, size_t len, uint8_t out_digest[NEI_SHA1_DIGEST_SIZE]) {
  nei_sha1_ctx_st ctx;
  nei_sha1_init(&ctx);
  nei_sha1_update(&ctx, data, len);
  nei_sha1_final(&ctx, out_digest);
}

void nei_sha1_to_hex(const uint8_t digest[NEI_SHA1_DIGEST_SIZE], char out_hex[NEI_SHA1_HEX_SIZE]) {
  static const char hex[] = "0123456789abcdef";
  size_t i;

  if (digest == NULL || out_hex == NULL) {
    return;
  }
  for (i = 0U; i < NEI_SHA1_DIGEST_SIZE; ++i) {
    out_hex[i * 2U] = hex[(digest[i] >> 4U) & 0x0FU];
    out_hex[i * 2U + 1U] = hex[digest[i] & 0x0FU];
  }
  out_hex[NEI_SHA1_HEX_SIZE - 1U] = '\0';
}

int nei_sha1_sum_hex(const void *data, size_t len, char out_hex[NEI_SHA1_HEX_SIZE]) {
  uint8_t digest[NEI_SHA1_DIGEST_SIZE];
  if (out_hex == NULL || (data == NULL && len > 0U)) {
    return -1;
  }
  nei_sha1_sum(data, len, digest);
  nei_sha1_to_hex(digest, out_hex);
  return 0;
}

int nei_sha1_file_sum(const char *file_path, uint8_t out_digest[NEI_SHA1_DIGEST_SIZE]) {
  FILE *fp;
  uint8_t buf[4096];
  nei_sha1_ctx_st ctx;
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

  nei_sha1_init(&ctx);
  for (;;) {
    n = fread(buf, 1U, sizeof(buf), fp);
    if (n > 0U) {
      nei_sha1_update(&ctx, buf, n);
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
  nei_sha1_final(&ctx, out_digest);
  return 0;
}

int nei_sha1_file_sum_hex(const char *file_path, char out_hex[NEI_SHA1_HEX_SIZE]) {
  uint8_t digest[NEI_SHA1_DIGEST_SIZE];
  if (out_hex == NULL) {
    return -1;
  }
  if (nei_sha1_file_sum(file_path, digest) != 0) {
    return -1;
  }
  nei_sha1_to_hex(digest, out_hex);
  return 0;
}
