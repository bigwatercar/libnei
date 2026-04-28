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

## 3. 总体架构

### 3.1 数据流

1. 业务线程调用 nei_llog/nei_vlog/nei_llog_literal/nei_vlog_literal
2. Producer 进行早过滤（level/verbose）
3. 事件序列化为紧凑二进制格式（header + payload）
4. 写入 MPSC ring（固定槽位，每槽一个 event buffer）
5. Consumer 线程按序消费并格式化文本
6. 按配置分发到 sinks 与可选 console

### 3.2 分层结构

- API 层
  - 对外函数、宏与配置结构
- 序列化层
  - printf 参数扫描、payload 编码、literal 直传
- 运行时层
  - MPSC ring、consumer 线程、flush 同步
- 格式化层
  - 时间戳、level tag、location、thread id 拼装
- sink 层
  - file sink（含批量写、周期 flush、轮转）与扩展 sink

### 3.3 关键设计原则

- 保证顺序一致性：write_pos/consumer_pos 单调推进，不允许“洞”
- 避免热路径锁竞争：producer 依赖原子操作 + TLS cache
- 控制栈占用：大 buffer 迁移到 TLS 或上下文结构
- 允许场景化折中：async 吞吐优先，strict sync 语义优先

## 4. 核心技术点

### 4.1 配置表与快照缓存

- 配置表容量固定（含 default 在内最多 16 个配置槽位）。
- 通过读写锁保护配置修改；通过 snapshot 版本号通知并发读路径。
- producer 侧使用 TLS 缓存整表指针：
  - 快路径：仅做 snapshot 读取与数组索引
  - 慢路径：在读锁下刷新 TLS 缓存

收益：在高频日志调用中显著降低锁开销。

### 4.2 早过滤（Early Filter）

nei_llog/nei_vlog 以及 literal 接口都在序列化前执行过滤：

- level 不启用则直接返回
- verbose 超阈值则直接返回

意义：避免无效日志进入序列化、入队、消费链路，减少 CPU 与内存带宽浪费。

### 4.3 序列化与格式计划

- 每条事件采用 header + payload 紧凑布局。
- payload 对常见类型使用固定 tag（i32/u32/i64/u64/double/ptr/cstr 等）。
- 对格式串扫描构建 fmt plan（TLS cache），命中时可复用操作序列。
- 无法安全计划时回退到扫描器路径；对危险 spec（如 %n）直接拒绝。
- literal 接口不走 printf 扫描，直接长度前缀复制消息体。

### 4.4 MPSC Ring 与消费者模型

- ring 槽位固定，producer 通过 fetch-add 预留槽位。
- 若槽未释放，producer 按“自旋 -> 让出 -> 条件变量等待”分级等待。
- consumer 顺序 drain：仅消费 committed 槽位，消费后释放槽。
- consumer 采用自适应空闲自旋：
  - 批量 drain 大时减少空转
  - 小批次/同步场景时保持更热态

### 4.5 Flush 语义

nei_log_flush 语义：等待调用前已入队事件被消费完成。

- 浅 backlog：优先短自旋快速返回
- 深 backlog：切换条件变量等待
- 在 consumer 线程内调用时直接 no-op，避免死锁

### 4.6 File Sink 设计

- 支持 append 写入、批量 buffer 聚合、按条数周期 fflush。
- Linux 优先 writev 路径，降低 syscall 次数与拷贝开销。
- 支持按文件大小轮转与备份链（.1/.2/...）。
- 通过环境变量可调 flush interval 与文件缓冲大小（用于 bench/调优）。

### 4.7 Crash Handler 与崩溃回溯日志

当前实现支持安装进程级 crash handler，并将崩溃信息同时输出到 stderr 与可选日志配置：

- API：nei_log_install_crash_handler(config_handle)
- Windows：通过 SetUnhandledExceptionFilter 捕获未处理异常
  - 回溯采集：CaptureStackBackTrace
  - 符号解析：DbgHelp（SymInitialize/SymFromAddr）
- Linux/POSIX：通过 sigaction 处理常见致命信号（SIGSEGV/SIGILL/SIGABRT/SIGFPE，若可用含 SIGBUS）
  - 回溯采集：backtrace/backtrace_symbols

关键行为：

- config_handle 可指定崩溃回溯写入哪个日志配置；传 NEI_LOG_INVALID_CONFIG_HANDLE 时仅写 stderr。
- 崩溃路径会调用 nei_llog_literal(..., NEI_L_FATAL, ...) 将每帧回溯写入日志队列，并在 handler 尾部执行 nei_log_flush 进行 best-effort 排空。
- 为避免 FATAL 日志触发 immediate_crash_on_fatal 递归崩溃，内部使用 s_in_crash_handler 抑制二次触发。

注意：POSIX 信号处理路径中的格式化与 flush 属于“崩溃前 best-effort”策略，不以严格 async-signal-safe 为目标。

### 4.8 Chromium 风格 Check 宏（nei/debug）

当前实现在 `modules/nei/debug/include/nei/debug/check.h` 提供了一组 Chromium 风格断言宏（宏名不带 `NEI_` 前缀）：

- CHECK(condition)
- CHECK_EQ/NE/LT/LE/GT/GE
- DCHECK(condition)
- DCHECK_EQ/NE/LT/LE/GT/GE

设计要点：

- 统一总开关：`NEI_CHROMIUM_LIKE_CHECK`
  - 默认值为 1；若设为 0，CHECK/DCHECK 系列均编译为 no-op。
- DCHECK 独立开关：`NEI_DCHECK_IS_ON`
  - 未显式定义时：Debug（未定义 `NDEBUG`）默认 1，Release（定义 `NDEBUG`）默认 0。
  - 当 `NEI_DCHECK_IS_ON=0` 时，DCHECK 系列完全裁剪为 no-op（不执行表达式）。
- 失败路径复用 log 能力：CHECK 失败会写 FATAL 日志、执行 `nei_log_flush()`，再 `abort()` 终止进程。

## 5. API 说明

### 5.1 配置 API

- nei_log_add_config
- nei_log_remove_config
- nei_log_get_config
- nei_log_default_config

关键配置项：

- level_flags / verbose_threshold
- timestamp_style
- short_level_tag / short_path
- log_location / log_location_after_message
- log_thread_id / log_to_console
- immediate_crash_on_fatal
- sinks

### 5.2 记录 API

- nei_llog（level + printf 风格）
- nei_vlog（verbose + printf 风格）
- nei_llog_literal（level + 预格式化字节串）
- nei_vlog_literal（verbose + 预格式化字节串）
- nei_log_flush
- nei_log_install_crash_handler

### 5.3 Sink API

- nei_log_default_file_sink_options
- nei_log_create_default_file_sink
- nei_log_destroy_sink

### 5.4 宏 API

- NEI_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL
- NEI_LOG（默认配置下的通用 level 宏）
- NEI_LOG_IF
- NEI_LOG_C（显式 config_handle 的通用 level 宏）
- NEI_LOG_C_IF
- NEI_LOG_VERBOSE
- NEI_LOG_VERBOSE_IF
- CHECK / CHECK_EQ / CHECK_NE / CHECK_LT / CHECK_LE / CHECK_GT / CHECK_GE
- DCHECK / DCHECK_EQ / DCHECK_NE / DCHECK_LT / DCHECK_LE / DCHECK_GT / DCHECK_GE

相关构建开关：

- NEI_CHROMIUM_LIKE_CHECK：统一启停 check 宏能力（默认开启）
- NEI_DCHECK_IS_ON：控制 DCHECK 是否参与编译（默认 Debug 开启、Release 关闭）

建议：

- 已在业务侧完成格式化时优先使用 literal 接口，减少重复格式化成本。
- 高频路径结合 level_flags/verbose_threshold 使用早过滤，避免无效日志负担。
- 多配置场景优先使用 NEI_LOG_C/NEI_LOG_C_IF，避免隐式落到默认配置。
- 若开启 immediate_crash_on_fatal，建议配合安装 crash handler，以便在崩溃前尽可能落盘回溯信息。
- 线上 Release 默认建议保持 `NEI_DCHECK_IS_ON=0`，仅保留 CHECK 作为强约束；排障版本可按需显式开启 DCHECK。

## 6. 性能与对比

### 6.1 最新结果摘要（2026-04-25）

详细数据见：docs/nei_log_benchmark_report_2026-04-25.md

摘要：

- 标准基准
  - Windows：整体无系统性回退，部分 file/literal 子项有波动
  - Linux：memory 场景整体提升；file 场景分化较明显
- compare 基准（NEI vs spdlog）
  - Linux memory：NEI 在 simple/literal 场景保持优势
  - per-call flush over async：spdlog 在两平台仍占优
  - strict sync flush：两者都被同步刷盘成本支配，环境敏感性强

### 6.2 结果解读原则

- 先看 us/log 趋势，再看 stddev 判断稳定性。
- memory 与 file 场景分开评估，不混用阈值。
- strict/sync 场景属于高抖动测试，建议增加 runs 并控制系统背景负载。

## 7. 遗留问题

1. Linux file 与 sync 部分子场景波动较大，稳定性不足。
2. per-call flush over async 场景与 spdlog 仍有明显差距。
3. strict sync 场景绝对延迟较高，且对环境依赖强。
4. 文件写路径策略（批量阈值、flush 策略）仍有进一步场景化空间。

## 8. 后续优化方向

1. 分场景调优 file sink 参数
- 将 flush_interval、batch_cap、stream buffer 形成预设档位
- 对低延迟与高吞吐分别给出默认模板

2. 降低 sync 请求链路开销
- 细化 flush 触发策略，减少不必要的 producer/consumer 同步等待
- 针对 literal 高频路径增加更短路径快车道

3. 加强跨平台可重复性
- 固化 benchmark 环境元数据
- 对 Linux/WSL 与原生 Linux 分开建立基线

4. 可观测性增强
- 扩展 perf stats（例如队列停留时间分位数）
- 增加配置维度与指标的关联输出

## 9. 维护建议

- 任何影响热路径的改动都应提供双平台 Release bench 与基线 Delta 表。
- 对格式化与序列化代码的“安全重构”优先级高于“激进微优化”。
- 若出现性能收益不稳定，优先保守回退并记录实验结论，再做定向复测。
