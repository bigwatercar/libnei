# neixx/log C++ 日志宏模块技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/log` C++ 便捷日志宏的设计目标、格式化后端选择、与 C 层 `nei/log` 的分工及调用链。

本文档基于：

- `modules/neixx/log/include/neixx/log/log.h`（全部宏定义）
- `modules/nei/log/include/nei/log/log.h`（C 层：`nei_llog_literal` / `nei_vlog_literal`）
- 参考 `nei_log_module_technical.md`（C 层 MPSC 无锁环形缓冲日志系统）

## 2. 模块定位

| 组件 | 定位 | 对标 Chromium |
|------|------|--------------|
| `NEIXX_LOG*` 宏族 | C++ 格式化日志（fmt/std::format） | `base/logging.h` 宏 |
| `nei/log`（C 层） | 传输/缓冲/多 sink（无锁环形缓冲） | `base::LogMessage` 后端 |

**分工**：neixx 层**只做格式化**，投递/缓冲/输出完全复用 C 层——两层共享同一套配置与 sink。

## 3. 格式化后端选择（编译期）

| 条件 | 后端 |
|------|------|
| `__has_include(<fmt/format.h>)` | `fmt::format`（C++17 默认路径） |
| C++20 且 `__has_include(<format>)` | `std::format` |
| 否则 | `#error`（编译期报错） |

```cpp
#define NEIXX_LOG_FMT(fmt_str, ...) fmt::format(fmt_str, ##__VA_ARGS__)
```

## 4. 宏清单与调用链

| 宏 | 语义 |
|----|------|
| `NEIXX_LOG(level, fmt, ...)` | 默认 config handle，级别化 |
| `NEIXX_LOG_IF(cond, level, fmt, ...)` | 条件日志（cond 只求值一次） |
| `NEIXX_LOG_C(handle, level, fmt, ...)` | 定向 config handle |
| `NEIXX_LOG_C_IF(cond, handle, level, fmt, ...)` | 条件 + 定向 |
| `NEIXX_LOG_V(verbose, fmt, ...)` / `_V_IF` | Verbose 子级别 |

```cpp
// NEIXX_LOG 展开：格式化 → C 层 literal 投递
#define NEIXX_LOG(level, fmt_str, ...)                                   \
  do {                                                                   \
    auto _neixx_msg_ = NEIXX_LOG_FMT(fmt_str, ##__VA_ARGS__);            \
    nei_llog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE, level,               \
                     __FILE__, __LINE__, NEI_FUNC,                       \
                     (_neixx_msg_).data(), (_neixx_msg_).size());        \
  } while (0)
```

- 定位信息（`__FILE__`/`__LINE__`/`NEI_FUNC`）在**宏调用点**捕获
- 投递走 C 层无锁环形缓冲（非阻塞，见 C 层文档）
- 受 `NEI_LOG_DISABLE_MACROS` 开关整体禁用

## 5. 使用范式

```cpp
#include <neixx/log/log.h>

NEIXX_LOG(NEI_L_INFO, "Hello {}", "world");
NEIXX_LOG_IF(x > 0, NEI_L_WARN, "x = {}", x);
NEIXX_LOG_C(custom_handle, NEI_L_ERROR, "targeted {}", detail);
NEIXX_LOG_V(2, "verbose {}", detail);
```

## 6. 设计要点

- **格式化与传输分离**：C++ 层 fmt 格式化（编译期后端选择），传输/缓冲复用 C 层
- **零状态宏封装**：无运行时对象，与 C 层配置天然互通
- **调用点定位**：文件/行/函数由宏捕获，非日志库线程采集
