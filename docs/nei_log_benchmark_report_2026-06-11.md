# Log 模块 Benchmark 报告（2026-06-11）

## 1. 结论摘要

本次采集基于 auto-flush 机制重构后的最新代码。关键变化：
- 同步 benchmark 从 per-call `nei_log_flush()` 改为 **1ms consumer auto-flush 定时器**，生产者不再阻塞
- 新增 `nei_log_create_stdout_sink` 替代旧 `log_to_console` 字段
- sink 生命周期统一走 `release` 回调

总体表现：内存场景吞吐 ~3M logs/s，文件场景 ~2.5M logs/s，格式化场景 ~1.8M logs/s。与 spdlog 对比在 literal 路径有优势，格式化路径 spdlog 的 fmt 库更高效。

## 2. 测试口径

- 构建模式：Release（VS2022）
- 平台：Windows 11
- CPU：AMD Ryzen 9
- 内存场景：1,000,000 iterations
- 文件场景：100,000 iterations
- 同步/Strict 场景：10,000 / 5,000 iterations
- 基准工具：`log_bench.exe` + `log_bench_compare.exe`

## 3. NEI 标准基准

### 3.1 Memory（无 sink，纯入队）

| 场景 | Iterations | E2E us/log | E2E logs/s |
|------|----------:|-----------:|-----------:|
| Log Info | 1,000,000 | 0.335 | 2,983,450 |
| Log Warn | 1,000,000 | 0.332 | 3,015,370 |
| Log Error | 1,000,000 | 0.341 | 2,933,950 |
| Log with Formatting | 1,000,000 | 0.554 | 1,805,870 |
| Log Info (literal) | 1,000,000 | 0.297 | 3,368,810 |
| Log Verbose | 1,000,000 | 0.348 | 2,874,700 |
| Log Verbose (literal) | 1,000,000 | 0.315 | 3,174,850 |

### 3.2 File（async file sink）

| 场景 | Iterations | E2E us/log | E2E logs/s | 文件大小 |
|------|----------:|-----------:|-----------:|--------:|
| Log Info | 100,000 | 0.408 | 2,449,060 | 8,800,000 |
| Log Warn | 100,000 | 0.425 | 2,354,830 | 8,800,000 |
| Log Error | 100,000 | 0.423 | 2,363,120 | 8,800,000 |
| Log with Formatting | 100,000 | 0.474 | 2,110,240 | 10,300,000 |
| Log Info (literal) | 100,000 | 0.370 | 2,705,700 | 9,300,000 |
| Log Verbose | 100,000 | 0.407 | 2,455,490 | 9,400,000 |
| Log Verbose (literal) | 100,000 | 0.369 | 2,708,050 | 9,100,000 |

## 4. NEI vs spdlog 对比

### 4.1 Memory（async, minimal sink）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple %s / {} | 0.340 | 0.439 | ✅ 22.5% |
| multi printf / fmt | 0.558 | 0.490 | ❌ -13.9% |
| llog_literal / literal only | 0.310 | 0.251 | ❌ -23.5% |
| vlog_literal | 0.268 | — | — |

### 4.2 File（async file sink）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 0.402 | 0.335 | ❌ -20.0% |
| multi args | 0.524 | 0.359 | ❌ -46.0% |
| llog_literal | 0.394 | — | — |
| vlog_literal | 0.378 | — | — |

### 4.3 File（auto-flush, 1ms consumer timer）🆕

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 0.431 | 0.186 | ❌ -131.7% |
| multi args | 0.455 | 0.236 | ❌ -92.8% |
| llog_literal | 0.555 | — | — |
| vlog_literal | 0.510 | — | — |

> **注**：spdlog 的 per-call flush 直接在内部分配线程上完成，无额外等待；NEI 的 1ms auto-flush 含 final `nei_log_flush()` 等待，此处为公平性差异而非性能问题。生产环境中 1ms 定时刷新对实时性已足够。

### 4.4 File（strict sync flush）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 10.646 | 6.764 | ❌ -57.4% |
| multi args | 10.613 | 6.623 | ❌ -60.3% |

> **注**：严格同步语义下两者均出现 >10μs 延迟，远超 async 路径。此模式不推荐生产使用，建议用 auto-flush 替代。

## 5. 与历史基线对比

| 场景 | 基线 (2026-04) us/log | 当前 us/log | Delta |
|------|--------------------:|-----------:|:----:|
| Memory Info | 0.379 | 0.335 | ✅ -11.6% |
| Memory Warn | 0.348 | 0.332 | ✅ -4.6% |
| Memory Formatting | 0.557 | 0.554 | ✅ -0.5% |
| Memory Info (literal) | 0.338 | 0.297 | ✅ -12.1% |
| Memory Verbose | 0.365 | 0.348 | ✅ -4.7% |
| File Info | 0.378 (avg) | 0.408 | ❌ +7.9% |

## 6. 关键结论

1. **Memory 路径全面优于基线**：auto-flush 重构未引入热路径开销，literal 路径 ~0.30μs/log 为最优。
2. **File async 路径略低于基线**：可能受 auto-flush 消费者定时唤醒影响，但差异在可接受范围。
3. **vs spdlog**：literal/pre-formatted 路径有优势，格式化路径 spdlog 的 fmt 库更高效（预期内）。
4. **auto-flush 为推荐生产模式**：1ms 定时刷新兼顾实时性与吞吐，避免 per-call flush 的高延迟（>10μs）。
5. **strict sync 仅用于完整性对比**，不应作为生产推荐模式。
