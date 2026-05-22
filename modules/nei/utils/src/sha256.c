#include <nei/utils/sha256.h>

#include <stdio.h>
#include <string.h>

#define NEI_SHA256_ROTR32(x, n) (((x) >> (n)) | ((x) << (32U - (n))))
#define NEI_SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define NEI_SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define NEI_SHA256_SIGMA0(x) (NEI_SHA256_ROTR32((x), 2U) ^ NEI_SHA256_ROTR32((x), 13U) ^ NEI_SHA256_ROTR32((x), 22U))
#define NEI_SHA256_SIGMA1(x) (NEI_SHA256_ROTR32((x), 6U) ^ NEI_SHA256_ROTR32((x), 11U) ^ NEI_SHA256_ROTR32((x), 25U))
#define NEI_SHA256_sigma0(x) (NEI_SHA256_ROTR32((x), 7U) ^ NEI_SHA256_ROTR32((x), 18U) ^ ((x) >> 3U))
#define NEI_SHA256_sigma1(x) (NEI_SHA256_ROTR32((x), 17U) ^ NEI_SHA256_ROTR32((x), 19U) ^ ((x) >> 10U))

static const uint32_t nei_sha256_k[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
    0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
    0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
    0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
    0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
    0xC67178F2U};

static uint32_t nei_sha256_load_u32_be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) | ((uint32_t)p[2] << 8U) | (uint32_t)p[3];
}

static void nei_sha256_store_u32_be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)((v >> 24U) & 0xFFU);
  p[1] = (uint8_t)((v >> 16U) & 0xFFU);
  p[2] = (uint8_t)((v >> 8U) & 0xFFU);
  p[3] = (uint8_t)(v & 0xFFU);
}

static void nei_sha256_transform(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t f;
  uint32_t g;
  uint32_t h;
  uint32_t i;

  for (i = 0U; i < 16U; ++i) {
    w[i] = nei_sha256_load_u32_be(block + i * 4U);
  }
  for (i = 16U; i < 64U; ++i) {
    w[i] = NEI_SHA256_sigma1(w[i - 2U]) + w[i - 7U] + NEI_SHA256_sigma0(w[i - 15U]) + w[i - 16U];
  }

  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];
  f = state[5];
  g = state[6];
  h = state[7];

  for (i = 0U; i < 64U; ++i) {
    const uint32_t t1 = h + NEI_SHA256_SIGMA1(e) + NEI_SHA256_CH(e, f, g) + nei_sha256_k[i] + w[i];
    const uint32_t t2 = NEI_SHA256_SIGMA0(a) + NEI_SHA256_MAJ(a, b, c);

    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void nei_sha256_init(nei_sha256_ctx_st *ctx) {
  if (ctx == NULL) {
    return;
  }
  ctx->state[0] = 0x6A09E667U;
  ctx->state[1] = 0xBB67AE85U;
  ctx->state[2] = 0x3C6EF372U;
  ctx->state[3] = 0xA54FF53AU;
  ctx->state[4] = 0x510E527FU;
  ctx->state[5] = 0x9B05688CU;
  ctx->state[6] = 0x1F83D9ABU;
  ctx->state[7] = 0x5BE0CD19U;
  ctx->total_len = 0ULL;
  ctx->buffer_len = 0U;
  memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void nei_sha256_update(nei_sha256_ctx_st *ctx, const void *data, size_t len) {
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
      nei_sha256_transform(ctx->state, ctx->buffer);
      ctx->buffer_len = 0U;
    }
  }

  while (offset + 64U <= len) {
    nei_sha256_transform(ctx->state, in + offset);
    offset += 64U;
  }

  if (offset < len) {
    const size_t remain = len - offset;
    memcpy(ctx->buffer, in + offset, remain);
    ctx->buffer_len = remain;
  }
}

void nei_sha256_final(nei_sha256_ctx_st *ctx, uint8_t out_digest[NEI_SHA256_DIGEST_SIZE]) {
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
    nei_sha256_update(ctx, pad, 56U - ctx->buffer_len);
  } else {
    nei_sha256_update(ctx, pad, 64U - ctx->buffer_len);
    nei_sha256_update(ctx, pad + 1U, 56U);
  }
  nei_sha256_update(ctx, len_bytes, 8U);

  for (i = 0U; i < 8U; ++i) {
    nei_sha256_store_u32_be(out_digest + i * 4U, ctx->state[i]);
  }
}

void nei_sha256_sum(const void *data, size_t len, uint8_t out_digest[NEI_SHA256_DIGEST_SIZE]) {
  nei_sha256_ctx_st ctx;
  nei_sha256_init(&ctx);
  nei_sha256_update(&ctx, data, len);
  nei_sha256_final(&ctx, out_digest);
}

void nei_sha256_to_hex(const uint8_t digest[NEI_SHA256_DIGEST_SIZE], char out_hex[NEI_SHA256_HEX_SIZE]) {
  static const char hex[] = "0123456789abcdef";
  size_t i;

  if (digest == NULL || out_hex == NULL) {
    return;
  }
  for (i = 0U; i < NEI_SHA256_DIGEST_SIZE; ++i) {
    out_hex[i * 2U] = hex[(digest[i] >> 4U) & 0x0FU];
    out_hex[i * 2U + 1U] = hex[digest[i] & 0x0FU];
  }
  out_hex[NEI_SHA256_HEX_SIZE - 1U] = '\0';
}

int nei_sha256_sum_hex(const void *data, size_t len, char out_hex[NEI_SHA256_HEX_SIZE]) {
  uint8_t digest[NEI_SHA256_DIGEST_SIZE];
  if (out_hex == NULL || (data == NULL && len > 0U)) {
    return -1;
  }
  nei_sha256_sum(data, len, digest);
  nei_sha256_to_hex(digest, out_hex);
  return 0;
}

int nei_sha256_file_sum(const char *file_path, uint8_t out_digest[NEI_SHA256_DIGEST_SIZE]) {
  FILE *fp;
  uint8_t buf[4096];
  nei_sha256_ctx_st ctx;
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

  nei_sha256_init(&ctx);
  for (;;) {
    n = fread(buf, 1U, sizeof(buf), fp);
    if (n > 0U) {
      nei_sha256_update(&ctx, buf, n);
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
  nei_sha256_final(&ctx, out_digest);
  return 0;
}

int nei_sha256_file_sum_hex(const char *file_path, char out_hex[NEI_SHA256_HEX_SIZE]) {
  uint8_t digest[NEI_SHA256_DIGEST_SIZE];
  if (out_hex == NULL) {
    return -1;
  }
  if (nei_sha256_file_sum(file_path, digest) != 0) {
    return -1;
  }
  nei_sha256_to_hex(digest, out_hex);
  return 0;
}