# nei/core 核心基础模块技术设计说明（C99）

## 1. 文档目标与范围

本文档描述 `nei/core` 七个子模块（时间/随机/路径/文件/字节序/编码/浮点控制）的设计目标、平台实现差异与关键坑。

本文档基于：

- `modules/nei/core/include/nei/core/{time,random,path_util,file_util,endian,encoding,float_ctrl}.h`
- `modules/nei/core/src/{time_posix.c,time_win.c?,random.c,path_util.c,file_util.c,endian.c,encoding.c,float_ctrl.c}`

## 2. time.h — 时间戳

| API | 时钟 | 实现 |
|-----|------|------|
| `nei_time_now_sec/ms/us` | 壁钟 | Win 系统时间 / POSIX `clock_gettime(CLOCK_REALTIME)` |
| `nei_time_now_ms_hires/us_hires` | 壁钟高精度 | 同上（POSIX 本身纳秒精度） |
| `nei_time_monotonic_ms` | 单调 | POSIX `clock_gettime(CLOCK_MONOTONIC)`（测间隔，不受墙钟调整影响） |

## 3. random.h — 加密安全随机

```c
int nei_random_buffer(void *out, size_t len);                 // 加密安全随机字节
int nei_random_string(char *out, size_t len, const char *charset);
```

- **Windows**：`BCryptGenRandom(NULL, out, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG)`
- **POSIX**：`open("/dev/urandom", O_RDONLY)` 读取

## 4. path_util.h — 跨平台路径

- `nei_path_join/dirname/basename/stem/extension/extensions/normalize/to_native`
- `nei_path_is_separator`：Windows 同时识别 `\` 与 `/`，POSIX 仅 `/`
- `nei_path_is_absolute`：Windows 含盘符（`C:`）与 UNC 前缀
- `nei_path_to_native`：就地转换分隔符
- 查询族：`exists/is_file/is_dir/is_readable`

## 5. file_util.h — 跨平台文件

- **`nei_fopen_utf8`**：UTF-8 路径 fopen。Windows 内部转 UTF-16 后 `_wfopen`（避免 ANSI 代码页乱码）；POSIX 直接 `fopen`
- `file_exists/size/remove/rename/truncate/append`：Windows 同样经 UTF-16 宽字符 API

## 6. endian.h — 字节序

- 判定：`nei_is_little_endian` / `nei_is_big_endian`
- 交换：`nei_bswap_u16/u32/u64`（含有符号）
- 主机↔网络序：`nei_htobe*` / `nei_be*toh`、主机↔小端 `nei_htole*` / `nei_le*toh`（u/i 全宽度）

## 7. encoding.h — 编码转换（重点坑）

```c
nei_wstr_to_utf8(src, src_len, buf, size);   // UTF-16 → UTF-8
nei_utf8_to_wstr(src, buf, size);            // UTF-8 → UTF-16
nei_mbcs_to_utf8(src, src_len, buf, size);   // 本地 MBCS → UTF-8
```

**Windows**：`WideCharToMultiByte(CP_UTF8)` / `MultiByteToWideChar(CP_UTF8)`，系统 API 已正确代理 surrogate pairs。注意空终止符计算边界（`needed` 是否含 `\0` 由 src_len 语义决定，实现内显式处理）。

**POSIX**：`sizeof(wchar_t)=4`（UTF-32）；**手动 UTF-8 解码必须显式支持 4 字节序列并生成 UTF-16 surrogate pairs**——只覆盖 1~3 字节的简单实现会损坏 emoji 等非 BMP 字符（库内已处理）。

## 8. float_ctrl.h — 浮点环境控制

- Windows：`_controlfp` 系列（舍入模式 / 异常掩码）
- POSIX：`fesetround` / `feenableexcept` 等 `<fenv.h>`
- `nei_float_ctrl_save/env`：保存/恢复浮点环境（跨模块调用保护）

---

## 9. 设计要点

- 全 C99、`NEI_API` 导出、Windows/POSIX 双实现（短分支同文件宏，长逻辑分文件）
- 路径/文件统一 UTF-8 输入，Windows 内部转 UTF-16
- 随机数走加密安全源（不用于可预测场景）
- 时间双时钟体系：壁钟（展示）+ 单调（测距）
