# nei/xdr 二进制序列化模块技术设计说明（C99）

## 1. 文档目标与范围

本文档描述 `nei/xdr` 二进制序列化原语的设计目标、线格式（字节序/对齐）、错误语义与使用范式。

本文档基于：

- `include/nei/xdr/xdr.h`（公开 API）
- `src/nei/xdr.c`（内部实现，纯 C99 + `<string.h>`）

## 2. 模块定位

| 定位 | 对标 |
|------|------|
| 轻量游标式二进制编码（有界缓冲区 writer/reader） | RFC 4506 XDR 子集 |

**设计哲学：** 无堆分配、无动态长度分配——调用方提供缓冲区，所有编码就地写入；变长数据
（bytes/string）以 `[4B LE 长度][数据][对齐 padding]` 自描述；所有读写带边界校验，
越界返回错误码，**无内存越界风险**。

## 3. 线格式

### 3.1 字节序：网络字节序（大端）

```c
// write_u32：高字节先写（big-endian）
buffer[off+0] = (value >> 24) & 0xFF;
buffer[off+1] = (value >> 16) & 0xFF;
buffer[off+2] = (value >>  8) & 0xFF;
buffer[off+3] = (value)       & 0xFF;
```

`i32` 由 `u32` 按位复用（补码二进制表示相同）；`float/double` 用 `memcpy` 按位复制后
按 `u32/u64` 编码（IEEE 754 大端）。

### 3.2 4 字节对齐 padding

```c
static uint32_t nei_xdr_padding(uint32_t length) {
  return (uint32_t)((4U - (length & 3U)) & 3U);   // 0..3，把长度补到 4 的倍数
}
```

opaque/bytes/string 的数据段后补 `0x00` 至 4 字节对齐（XDR 兼容布局，便于 32 位直接寻址）。

### 3.3 三种复合类型语义

| 函数 | 线格式 |
|------|--------|
| `write_opaque` | `[原始数据][对齐 padding]`（**定长，无长度前缀**） |
| `write_bytes` | `[4B 长度][原始数据][对齐 padding]`（**变长，带长度前缀**） |
| `write_string` | 同 `write_bytes`（长度 = 字节数） |

> ⚠️ 易混淆点：`opaque` 是**定长**（长度由调用者给定，解码侧必须知道），
> `bytes` 才是**变长自描述**（长度前缀）。`write_bytes` 实现即
> `write_u32(length)` + `write_opaque(data, length)`。

## 4. 错误语义

```c
enum nei_xdr_status_e {
  NEI_XDR_OK = 0,
  NEI_XDR_EINVAL = -1,   // NULL 指针 / 参数非法
  NEI_XDR_EBOUNDS = -2   // 缓冲区越界
};
```

- 每次写入/读取前经 `nei_xdr_has_space(size, offset, need)` 边界校验（含 offset>size 防溢出）
- `read_bytes/read_string` 额外校验 `out_capacity`，解码侧可拒绝超长数据
- 越界**原子失败**：offset 不推进，游标保持在操作前位置

## 5. 状态与游标

```c
struct nei_xdr_writer_st { uint8_t *buffer; size_t size; size_t offset; };
struct nei_xdr_reader_st { const uint8_t *buffer; size_t size; size_t offset; };
```

- `tell()` / `remaining()` 支持增量组装与流式消费
- `skip_opaque` / `skip_bytes` 供解码侧跳过不关心字段（不拷贝）

## 6. 使用范式

```c
uint8_t buf[256];
struct nei_xdr_writer_st w;
nei_xdr_writer_init(&w, buf, sizeof(buf));
nei_xdr_write_u32(&w, 42);
nei_xdr_write_string(&w, "hello", 5);      // 4B 长度 + 数据 + 对齐
nei_xdr_write_u64(&w, UINT64_MAX);

struct nei_xdr_reader_st r;
nei_xdr_reader_init(&r, buf, nei_xdr_writer_tell(&w));
uint32_t n = 0;
nei_xdr_read_u32(&r, &n);                  // 42
```

## 7. 设计要点

- **大端网络序**：跨端序/跨平台稳定（配套 `nei/core/endian.h` 本地转换）
- **对齐与自描述**：变长类型带长度前缀，padding 对齐——XDR 兼容
- **有界安全**：全部操作边界校验 + 原子失败，适合解析不可信输入
- **零分配**：纯栈/调用方缓冲，无 malloc
