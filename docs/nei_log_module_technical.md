# Log 模块技术设计说明

## 1. 文档目标与范围

本文档面向 NEI log 模块的使用方与维护方，说明当前版本的：

- 总体设计与架构分层
- 核心技术点与关键数据结构
- API 能力与使用建议
- 性能表现与对比结论
- 已知遗留问题与后续优化方向

本文聚焦当前可用实现，不展开历史演化细节。

## 2. 模块定位

log 模块是 C 语言层的高性能异步日志基础设施，目标是：

- 保持热路径低开销（producer 侧快速返回）
- 提供跨平台一致语义（Windows/Linux）
- 支持配置化输出（等级、时间戳、位置、线程 ID、sink）
- 在需要时提供可诊断性（perf counters、flush 语义）
- 事件自包含（deep-copy header），消除 DLL/SO 卸载悬空指针风险

## 3. 总体架构

### 3.1 数据流

1. 业务线程调用 nei_llog / nei_vlog / nei_llog_literal / nei_vlog_literal
2. Producer 进行早过滤（level / verbose / invalid handle）
3. 事件序列化为紧凑二进制格式（header + payload），header 中深拷贝源位置字符串
4. 写入 MPSC ring buffer（固定 256 槽位，每槽一个自包含事件）
5. Consumer 线程按序消费并格式化文本
6. 按配置分发到 sinks

### 3.2 分层结构

- **API 层** — 对外函数、宏与配置结构
- **序列化层** — printf 参数扫描、payload 编码、literal 直传、源位置深拷贝
- **运行时层** — MPSC ring buffer、consumer 线程、flush 同步
- **格式化层** — 时间戳、level tag、location、thread id 拼装
- **Sink 层** — file sink（含批量写、周期 flush、轮转）与自定义 sink

### 3.3 关键设计原则

- 保证顺序一致性：write_pos / consumer_pos 单调推进
- 避免热路径锁竞争：producer 依赖原子操作 + TLS cache
- 事件自包含：序列化时深拷贝源位置字符串，消除跨异步边界的指针依赖
- 控制栈占用：大 buffer 迁移到 TLS 或上下文结构
- 允许场景化折中：async 吞吐优先，sync 语义优先

## 4. 核心技术点

### 4.1 配置表与快照缓存

- 配置表容量固定（含 default 在内最多 16 个配置槽位）。
- 通过读写锁保护配置修改；通过 snapshot 版本号通知并发读路径。
- `nei_log_get_config()` / `nei_log_default_config()` 返回可原地修改的配置指针；修改后**必须**调用 `nei_log_update_config()` 使变更对日志生产者线程可见（触发 snapshot bump）。
- `nei_log_default_config()` 快路径仅获取读锁（双重检查锁定，仅首次初始化需写锁）。
- producer 侧使用 TLS 缓存整表指针：
  - 快路径：仅做 snapshot 读取与数组索引
  - 慢路径：在读锁下刷新 TLS 缓存

收益：在高频日志调用中显著降低锁开销，默认配置读取不再阻塞日志记录。

### 4.2 早过滤（Early Filter）

nei_llog / nei_vlog / literal 接口都在序列化前执行过滤：

- 无效 config handle 直接返回（无序列化消耗）
- level 不启用则直接返回
- verbose 超阈值则直接返回

意义：避免无效日志进入序列化、入队、消费链路，减少 CPU 与内存带宽浪费。

### 4.3 事件序列化与源位置深拷贝

- 每条事件采用 header + payload 紧凑布局。
- Header 中 `file`、`func`、`fmt` 字段为 inline char buffer（128/160/112 字节），序列化时深拷贝。
  - 消除 DLL/SO 卸载场景下的悬空指针风险：事件写入 ring buffer 后不再持有外部指针。
  - Header 字段按对齐要求排列（`total_size` 在 offset 0），`_reserved` padding 消除内部空洞。
- payload 对常见类型使用固定 tag（i32/u32/i64/u64/double/ptr/cstr/longdouble/literal_msg）。
- 对格式串扫描构建 fmt plan（TLS cache，64 操作上限），命中时可复用操作序列。
- 无法安全计划时回退到扫描器路径；对危险 spec（如 `%n`）直接拒绝。
- literal 接口不走 printf 扫描，直接长度前缀复制消息体（`_NEI_LOG_PAYLOAD_LITERAL_MSG`）。
- `z` 和 `t` 长度修饰符在所有支持 ABI 上宽度相同，共享单分支处理（带注释说明）。

### 4.4 MPSC Ring Buffer 与消费者模型

- 256 个固定槽位（可通过 CMake `NEI_LOG_RING_SLOTS` 覆盖），每槽有一个 atomic state 标志。
- producer 通过 `fetch_add` 预留槽位，自旋等待 consumer 释放，写入后 store-release 提交。
- consumer 顺序 drain：仅消费已提交槽位，消费后重置 state 为 0 释放槽位。
- consumer 自适应空闲自旋：
  - 大批量 drain（>=16）：减少自旋（128 轮）
  - 中等 drain（>=4）：中等自旋（256 轮）
  - 小批量 / 同步：最大自旋（512 轮）
- wake-up 策略：producer 仅在 consumer 已标记 sleeping 时通过条件变量唤醒

### 4.5 Flush 语义

`nei_log_flush` 语义：等待调用前已入队事件被消费完成。

- 浅 backlog（<=2）：积极自旋 4096 轮快速返回
- 中 backlog（<=8）：中等自旋 2048 轮
- 深 backlog：保守自旋 1024 轮后切换条件变量等待
- 在 consumer 线程内调用时直接 no-op，避免死锁

注意：若某 producer 在 `fetch_add` 预留槽位后被抢占，flush 会等待该 producer 恢复并提交，这是设计行为（不允许"洞"）。

### 4.6 File Sink 与自定义 Sink 生命周期

- 支持 append 写入、批量 buffer 聚合（默认 64KB）、按条数周期 fflush（默认 256 条）。
- Linux 优先 writev 路径，降低 syscall 次数与拷贝开销。
- 支持按文件大小轮转与备份链（.1/.2/...）。
- 通过环境变量可调 flush interval 与文件缓冲大小（用于 bench/调优）。
- 文件流 buffer 与批量写 buffer 互斥：启用批量写时禁用流缓冲（避免双重缓冲）。

**定时自动刷新**：通过 `nei_log_set_auto_flush_interval_ms()` 可配置消费者线程的定时唤醒间隔。消费者空闲时按此间隔自动刷新所有 file sink 的缓冲数据，确保 `tail -f` 等外部工具能实时看到日志输出（默认禁用，推荐值 1000ms）。

**Sink release 回调**：`nei_log_sink_st::release` 是可选的资源清理回调。当配置被 `nei_log_remove_config` 移除时，库会遍历该配置的所有 sink 并调用其 `release` 回调。内置 sink（file sink、stdout sink）的 release 会释放所有内部资源及 sink 结构体本身。自定义 sink 应在 release 中释放 `opaque` 及自有资源。

### 4.7 Crash Handler 与崩溃回溯日志

当前实现支持安装进程级 crash handler，并将崩溃信息同时输出到 stderr 与可选日志配置：

- API：`nei_log_install_crash_handler(config_handle)`
- Windows：通过 `SetUnhandledExceptionFilter` 捕获未处理异常
  - 回溯采集：`CaptureStackBackTrace`
  - 符号解析：`DbgHelp`（`SymInitialize` / `SymFromAddr`）
- Linux/POSIX：通过 `sigaction`（`SA_RESETHAND`）处理常见致命信号
  - 覆盖信号：`SIGSEGV` / `SIGILL` / `SIGABRT` / `SIGFPE`（若可用含 `SIGBUS`）
  - 回溯采集：`backtrace` / `backtrace_symbols`

关键行为：

- `config_handle` 可指定崩溃回溯写入哪个日志配置；传 `NEI_LOG_INVALID_CONFIG_HANDLE` 时仅写 stderr。
- 崩溃路径会调用 `nei_llog_literal(..., NEI_L_FATAL, ...)` 将每帧回溯写入日志队列，并在 handler 尾部执行 `nei_log_flush` 进行 best-effort 排空。
- `s_in_crash_handler` 为 `volatile sig_atomic_t`，确保信号处理器写入对普通线程可见，抑制 `immediate_crash_on_fatal` 递归崩溃。
- **已知限制（P0）**：POSIX 信号处理器中调用了 `nei_llog_literal`、`nei_log_flush`、`backtrace_symbols`（malloc）、`snprintf` 等非异步信号安全函数。若被中断的线程持有 `s_runtime.mutex` 或 `s_config_lock`，存在死锁风险。当前作为"崩溃前 best-effort"策略接受，不以严格 async-signal-safe 为目标。

### 4.8 Chromium 风格 Check 宏（nei/debug）

当前实现在 `include/nei/debug/check.h` 提供了一组 Chromium 风格断言宏：

- `CHECK(condition)` / `CHECK_EQ` / `CHECK_NE` / `CHECK_LT` / `CHECK_LE` / `CHECK_GT` / `CHECK_GE`
- `DCHECK(condition)` / `DCHECK_EQ` / `DCHECK_NE` / `DCHECK_LT` / `DCHECK_LE` / `DCHECK_GT` / `DCHECK_GE`

设计要点：

- 统一总开关：`NEI_CHROMIUM_LIKE_CHECK`（默认 1）
- DCHECK 独立开关：`NEI_DCHECK_IS_ON`（Debug 默认 1，Release 默认 0）
- 失败路径复用 log 能力：CHECK 失败写 FATAL 日志 -> `nei_log_flush()` -> `abort()`

### 4.9 编码与可移植性

- 宽字符串转码：Windows 使用 `WideCharToMultiByte(CP_UTF8, ...)`，POSIX 使用 `iconv("UTF-8", "WCHAR_T", ...)`。两端统一 UTF-8 输出，消除系统 locale 依赖。
- 线程 ID：
  - Windows：`GetCurrentThreadId()` 格式化为 `%lu`
  - POSIX：`pthread_t` 按字节序感知的十六进制逐字节打印（避免将不透明类型强转为 `unsigned long` 的未定义行为）
- 时间戳 TLS 缓存：监测 NTP 回拨（`sec < cache->sec`），自动 invalidate 缓存防止输出过期时间戳。

## 5. API 说明

### 5.1 配置 API

| API | 说明 |
|-----|------|
| `nei_log_add_config` | 添加配置，返回 handle |
| `nei_log_remove_config` | 按 handle 移除配置（已含 sink 释放，**勿**再手动 destroy 其 sink） |
| `nei_log_shutdown` | 移除所有配置并停止消费者线程，释放全部资源 |
| `nei_log_update_config` | 发布原地修改，使配置变更对所有线程生效 |
| `nei_log_add_sink` | 将 sink 插入配置的 sinks 数组首个空位 |
| `nei_log_remove_sink` | 从 sinks 数组中移除指定 sink（不释放，紧凑数组） |
| `nei_log_get_config` | 按 handle 获取可修改的配置指针（修改后须调用 `nei_log_update_config`） |
| `nei_log_default_config` | 获取默认配置（slot 0，修改后须调用 `nei_log_update_config`） |

关键配置项（`nei_log_config_st`）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `level_flags` | `nei_log_level_flags_u` | 各 level 的启用位 |
| `verbose_threshold` | `int32_t` | verbose 上限（-1 = 不过滤） |
| `timestamp_style` | `nei_log_timestamp_style_e` | 时间戳格式 |
| `short_level_tag` | `uint32_t:1` | 是否用短 level 标签 |
| `short_path` | `uint32_t:1` | 是否仅输出文件名 |
| `log_location` | `uint32_t:1` | 是否输出源位置 |
| `log_location_after_message` | `uint32_t:1` | 源位置放消息前/后 |
| `log_thread_id` | `uint32_t:1` | 是否输出 `tid=` |
| `immediate_crash_on_fatal` | `uint32_t:1` | FATAL 日志后立即崩溃 |
| `sinks` | `nei_log_sink_st*[8]` | sink 数组（NULL 终止）。sink 可设置 `release` 回调由库在 remove/destroy 时调用 |

### 5.2 记录 API

| API | 说明 |
|-----|------|
| `nei_llog(handle, level, file, line, func, fmt, ...)` | printf 风格日志 |
| `nei_vlog(handle, verbose, file, line, func, fmt, ...)` | verbose 子级日志 |
| `nei_llog_literal(handle, level, file, line, func, msg, len)` | 预格式化日志 |
| `nei_vlog_literal(handle, verbose, file, line, func, msg, len)` | 预格式化 verbose |
| `nei_log_flush()` | 等待所有已入队事件消费完成 |
| `nei_log_install_crash_handler(handle)` | 安装崩溃处理器 |

### 5.3 Sink API

| API | 说明 |
|-----|------|
| `nei_log_default_file_sink_options()` | 获取默认 file sink 选项 |
| `nei_log_create_default_file_sink(path, opts)` | 创建内置 file sink |
| `nei_log_create_stdout_sink()` | 创建内置 stdout sink |
| `nei_log_release_sink(sink)` | 调用 sink 的 release 回调释放资源 |

### 5.4 运行时 API

| API | 说明 |
|-----|------|
| `nei_log_flush()` | 等待所有已入队事件消费完成 |
| `nei_log_set_auto_flush_interval_ms(ms)` | 设置定时自动刷新间隔（默认 0=禁用，推荐 1000） |
| `nei_log_get_auto_flush_interval_ms()` | 获取当前定时刷新间隔 |
| `nei_log_shutdown()` | 移除所有配置并停止消费者线程，释放全部资源 |
| `nei_log_install_crash_handler(handle)` | 安装崩溃处理器 |

### 5.5 宏 API

- `NEI_LOG_TRACE` / `NEI_LOG_DEBUG` / `NEI_LOG_INFO` / `NEI_LOG_WARN` / `NEI_LOG_ERROR` / `NEI_LOG_FATAL`
- `NEI_LOG` / `NEI_LOG_IF` -- 默认配置的通用 level 宏
- `NEI_LOG_C` / `NEI_LOG_C_IF` -- 显式 config_handle 的通用 level 宏
- `NEI_LOG_VERBOSE` / `NEI_LOG_VERBOSE_IF` -- verbose 宏
- `CHECK` / `CHECK_EQ` / `CHECK_NE` / `CHECK_LT` / `CHECK_LE` / `CHECK_GT` / `CHECK_GE`
- `DCHECK` / `DCHECK_EQ` / `DCHECK_NE` / `DCHECK_LT` / `DCHECK_LE` / `DCHECK_GT` / `DCHECK_GE`

### 5.5 使用建议

- 已在业务侧完成格式化时优先使用 literal 接口，减少重复格式化成本。
- 高频路径结合 `level_flags` / `verbose_threshold` 使用早过滤。
- 多配置场景优先使用 `NEI_LOG_C` / `NEI_LOG_C_IF`，避免隐式落到默认配置。
- 若开启 `immediate_crash_on_fatal`，建议配合安装 crash handler。
- 线上 Release 默认保持 `NEI_DCHECK_IS_ON=0`，仅保留 CHECK 作为强约束。

## 6. 关键数据结构

### 6.1 事件头部（nei_log_event_header_st）

```
offset  size  field
0       4     total_size
4       4     (padding to align timestamp_ns)
8       8     timestamp_ns
16      8     config_handle
24      4     level
28      4     line
32      4     verbose
36      1     thread_id_len
37      1     is_literal
38      23    thread_id_str
61      3     _reserved (padding, absorbs trailing waste)
64      128   file
192     160   func
352     112   fmt
--- sizeof = 464, zero internal holes
```

### 6.2 Ring Slot

```c
typedef struct {
  _nei_log_atomic32_t state;  // 0=empty, 1=committed
  uint32_t            size;
  uint8_t             data[_NEI_LOG_EVENT_BUFFER_SIZE]; // 8192
} nei_log_ring_slot_st;
```

### 6.3 运行时结构

```c
typedef struct _nei_log_runtime_st {
  nei_log_ring_st ring;                     // 256 slot x 8KB
  _nei_log_atomic64_t stat_producer_spin_loops;
  _nei_log_atomic64_t stat_flush_wait_loops;
  _nei_log_atomic64_t stat_consumer_wakeups;
  _nei_log_atomic64_t stat_ring_high_watermark;
  _nei_log_atomic32_t consumer_sleeping;
  int stop_requested, initialized;
  // + platform-specific mutex/cond/thread
} nei_log_runtime_st;
```

## 7. 性能参考（2026-06-11 Windows Release）

| 场景 | enqueue us/log | 吞吐 logs/s |
|------|---------------|-------------|
| Memory Info | 0.637 | 1,570,260 |
| Memory Literal | 0.534 | 1,871,350 |
| Memory Format (multi-arg) | 1.059 | 944,259 |
| File Info (SSD) | 0.738 | 1,354,790 |
| File Literal (SSD) | 0.696 | 1,437,220 |

## 8. 遗留问题

### 8.1 P0 -- 信号处理器非异步信号安全

POSIX 信号处理器中调用了 `nei_llog_literal`、`nei_log_flush`、`backtrace_symbols`（malloc）、
`snprintf` 等非异步信号安全函数。若被中断线程持有 `s_runtime.mutex` 或 `s_config_lock`，
存在死锁风险。Windows 端 `SymFromAddr` 内部持有 DbgHelp 锁。

- **影响**：崩溃处理器自身可能死锁（概率低但后果严重）
- **缓解**：`s_in_crash_handler` 为 `volatile sig_atomic_t` + `SA_RESETHAND` 单次触发
- **状态**：接受为"崩溃前 best-effort"策略，待后续架构级修复

### 8.2 性能相关

1. Linux file 与 sync 部分子场景波动较大，稳定性不足。
2. per-call flush over async 场景与 spdlog 仍有差距。
3. strict sync 场景绝对延迟较高，且环境依赖强。
4. 文件写路径策略仍有进一步场景化空间。

## 9. 后续优化方向

1. **分场景调优 file sink 参数** -- 将 flush_interval、batch_cap、stream buffer 形成预设档位
2. **降低 sync 请求链路开销** -- 细化 flush 触发策略
3. **加强跨平台可重复性** -- 固化 benchmark 环境元数据
4. **可观测性增强** -- 扩展 perf stats（队列停留时间分位数）
5. **信号安全崩溃处理器** -- crash handler 改为纯 `write()+stderr`，`backtrace_symbols_fd()` 替代 `backtrace_symbols()`

## 10. 维护建议

- 任何影响热路径的改动都应提供双平台 Release bench 与基线 Delta 表。
- 对格式化与序列化代码的"安全重构"优先级高于"激进微优化"。
- 若出现性能收益不稳定，优先保守回退并记录实验结论。
- 新增配置字段需同步更新 `_nei_log_fill_default_config` 和本文档 sec.5.1。
