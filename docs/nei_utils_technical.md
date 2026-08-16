# nei/utils 工具算法模块技术设计说明（C99）

## 1. 文档目标与范围

本文档描述 `nei/utils` 编码/哈希/ID 生成算法的设计目标、接口约定与实现要点。

本文档基于：

- `modules/nei/utils/include/nei/utils/{base64,crc32,md5,sha1,sha256,uuid,flake_id}.h`
- `modules/nei/utils/src/*.c`（实现）

## 2. 模块总览

```
nei/utils/
├── base64.h    — Base64 编码/解码（长度预计算）
├── crc32.h     — CRC-32
├── md5.h       — MD5（16B）
├── sha1.h      — SHA-1（20B）
├── sha256.h    — SHA-256（32B）
├── uuid.h      — UUID v4（加密安全随机）
└── flake_id.h  — flake 风格分布式唯一 ID
```

## 3. base64.h

```c
size_t nei_base64_encoded_length(size_t input_len);        // 预计算输出长度
size_t nei_base64_decoded_max_length(size_t input_len);
int nei_base64_encode(const uint8_t *, size_t, char *out, size_t out_cap, size_t *out_len);
int nei_base64_decode(const char *, size_t, uint8_t *out, size_t out_cap, size_t *out_len);
```

- 编码/解码均带 `out_cap` 容量保护，返回实际长度

## 4. 哈希家族（crc32/md5/sha1/sha256）统一 ctx 约定

```c
// 流式（大文件/分块）
void nei_<algo>_init(nei_<algo>_ctx_st *ctx);
void nei_<algo>_update(ctx, const void *data, size_t len);
<out> nei_<algo>_final(ctx, uint8_t out_digest[N]);   // crc32: uint32_t 返回

// 一次性（小数据）
<out> nei_<algo>_sum(const void *data, size_t len, uint8_t out_digest[N]);
int  nei_<algo>_sum_hex(const void *data, size_t len, char out_hex[HEX_SIZE]);
void nei_<algo>_to_hex(const uint8_t digest[N], char out_hex[HEX_SIZE]);
```

摘要/hex 尺寸：MD5 16B/33、SHA-1 20B/41、SHA-256 32B/65、CRC-32 4B/9。

## 5. uuid.h（UUID v4）

```c
int nei_uuid4_generate(uint8_t out[16]);             // 加密安全随机 + v4 版本/变体位
int nei_uuid_to_string(const uint8_t uuid[16], char out[37]);
int nei_uuid4_generate_string(char out[37]);
int nei_uuid_from_string(const char *str, uint8_t out[16]);
int nei_uuid_compare(const uint8_t a[16], const uint8_t b[16]);
```

- 随机源复用 `nei/core/random.h`（BCryptGenRandom / /dev/urandom）

## 6. flake_id.h（分布式唯一 ID）

```c
uint64_t nei_flake_next_id(void);
```

- 布局：`[41-bit 时间戳][machine/seq 位]`，epoch `NEI_FLAKE_EPOCH_MS`
- 单进程内单调递增；跨进程按 machine-id 区分（Windows/POSIX 各自机器标识）

## 7. 设计要点

- 全 C99、`NEI_API` 导出、无外部依赖（自研实现）
- 哈希统一 `init/update/final` + `sum` + `sum_hex` 三档接口
- UUID v4 与随机数均基于加密安全源
