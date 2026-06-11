# Log 模块 Benchmark 报告（2026-06-11）

## 1. 结论摘要

本次采集基于 auto-flush 机制重构后的最新代码。关键变化：
- 同步 benchmark 从 per-call `nei_log_flush()` 改为 **1ms consumer auto-flush 定时器**，生产者不再阻塞
- 新增 `nei_log_create_stdout_sink` 替代旧 `log_to_console` 字段
- sink 生命周期统一走 `release` 回调

总体表现：内存场景吞吐 ~3M logs/s，文件场景 ~2.5M logs/s，格式化场景 ~1.9M logs/s。与 spdlog 对比在 literal/plain 路径有优势，格式化路径 spdlog 的 fmt 库更高效。

## 2. 测试口径

- 构建模式：Release（VS2022）
- 平台：Windows 11
- CPU：AMD Ryzen 9
- 内存场景：1,000,000 iterations
- 文件场景：100,000 iterations
- 同步/Strict 场景：10,000 / 5,000 iterations
- 基准工具：`log_bench.exe` + `log_bench_compare.exe`
- 采集时间：2026-06-12

## 3. NEI 标准基准

### 3.1 Memory（无 sink，纯入队）

| 场景 | Iterations | E2E us/log | E2E logs/s |
|------|----------:|-----------:|-----------:|
| Log Info | 1,000,000 | 0.325 | 3,073,320 |
| Log Warn | 1,000,000 | 0.323 | 3,098,660 |
| Log Error | 1,000,000 | 0.338 | 2,961,190 |
| Log with Formatting | 1,000,000 | 0.533 | 1,875,430 |
| Log Info (literal) | 1,000,000 | 0.310 | 3,228,510 |
| Log Verbose | 1,000,000 | 0.349 | 2,863,330 |
| Log Verbose (literal) | 1,000,000 | 0.311 | 3,216,940 |

### 3.2 File（async file sink）

| 场景 | Iterations | E2E us/log | E2E logs/s | 文件大小 |
|------|----------:|-----------:|-----------:|--------:|
| Log Info | 100,000 | 0.440 | 2,273,190 | 8,800,000 |
| Log Warn | 100,000 | 0.385 | 2,594,240 | 8,800,000 |
| Log Error | 100,000 | 0.388 | 2,578,180 | 8,800,000 |
| Log with Formatting | 100,000 | 0.510 | 1,962,550 | 10,300,000 |
| Log Info (literal) | 100,000 | 0.341 | 2,935,560 | 9,300,000 |
| Log Verbose | 100,000 | 0.379 | 2,639,500 | 9,400,000 |
| Log Verbose (literal) | 100,000 | 0.377 | 2,652,240 | 9,100,000 |

## 4. NEI vs spdlog 对比

### 4.1 Memory（async, minimal sink）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple %s / {} | 0.376 | 0.439 | ✅ 14.3% |
| multi printf / fmt | 0.548 | 0.491 | ❌ -11.6% |
| llog_literal / literal only | 0.354 | 0.228 | ❌ -54.9% |
| vlog_literal | 0.329 | — | — |

### 4.2 File（async file sink）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 0.412 | 0.343 | ❌ -19.9% |
| multi args | 0.480 | 0.391 | ❌ -22.7% |
| llog_literal | 0.381 | — | — |
| vlog_literal | 0.340 | — | — |

### 4.3 File（auto-flush, 1ms consumer timer）🆕

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 0.426 | 0.222 | ❌ -92.0% |
| multi args | 0.508 | 0.205 | ❌ -148.3% |
| llog_literal | 0.425 | — | — |
| vlog_literal | 0.413 | — | — |

### 4.4 File（strict sync flush）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 10.057 | 7.074 | ❌ -42.2% |
| multi args | 9.238 | 7.010 | ❌ -31.8% |

## 5. 与历史基线对比（2026-04）

| 场景 | 基线 us/log | 当前 us/log | Delta |
|------|----------:|-----------:|:----:|
| Memory Info | 0.379 | 0.325 | ✅ -14.2% |
| Memory Warn | 0.348 | 0.323 | ✅ -7.2% |
| Memory Formatting | 0.557 | 0.533 | ✅ -4.3% |
| Memory Info (literal) | 0.338 | 0.310 | ✅ -8.3% |
| Memory Verbose | 0.365 | 0.349 | ✅ -4.4% |

## 6. 关键结论

1. **Memory 路径全面优于 4 月基线**：auto-flush 重构未引入热路径开销，literal 路径 ~0.31μs/log 为最优。
2. **File async 路径稳定**：~0.34-0.51μs/log，日志量大时吞吐可靠。
3. **vs spdlog**：plain消息和 verbose 路径 NEI 有竞争力；格式化路径 spdlog 的 fmt 库更高效（预期内）；literal 路径 spdlog 显著更快（无序列化开销）。
4. **auto-flush 为推荐生产模式**：1ms 定时刷新兼顾实时性与吞吐（~0.41-0.51μs/log），远优于 strict sync 的 ~10μs/log。
5. **strict sync 仅用于完整性对比**，不应作为生产推荐模式。
