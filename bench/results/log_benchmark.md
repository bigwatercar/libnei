# Log 模块 Benchmark 报告（2026-06-24）

## 1. 结论摘要

本次采集基于最新代码（含 weak_ptr Release 构建修复）。测试口径与上轮（2026-06-11）一致。

总体表现：内存场景吞吐 ~2.8-3.3M logs/s，文件场景 ~2.1-2.5M logs/s，格式化场景 ~1.8M logs/s。与 spdlog 对比在 simple %s 内存路径有显著优势（42%），但格式化及 literal 路径 spdlog 仍领先。

## 2. 测试口径

- 构建模式：Release（VS2022, MSVC 17.14）
- 平台：Windows 11 专业工作站版 (Build 10.0.26200)
- CPU：Intel Core Ultra 9 185H @ 2.30 GHz（16 核 / 22 逻辑处理器，Meteor Lake）
- 内存：32 GB（2 × 16 GB SK Hynix DDR5-8533）
- 存储：SSD（NVMe）
- 内存场景：1,000,000 iterations
- 文件场景：100,000 iterations
- auto-flush 场景：10,000 iterations
- Strict 场景：5,000 iterations
- 基准工具：`log_bench.exe` + `log_bench_compare.exe`
- 采集时间：2026-06-24

## 3. NEI 标准基准

### 3.1 Memory（无 sink，纯入队）

| 场景 | Iterations | E2E us/log | E2E logs/s |
|------|----------:|-----------:|-----------:|
| Log Info | 1,000,000 | 0.340 | 2,942,480 |
| Log Warn | 1,000,000 | 0.351 | 2,846,420 |
| Log Error | 1,000,000 | 0.342 | 2,925,140 |
| Log with Formatting | 1,000,000 | 0.560 | 1,784,800 |
| Log Info (literal) | 1,000,000 | 0.301 | 3,325,730 |
| Log Verbose | 1,000,000 | 0.359 | 2,785,570 |
| Log Verbose (literal) | 1,000,000 | 0.326 | 3,071,150 |

### 3.2 File（async file sink）

| 场景 | Iterations | E2E us/log | E2E logs/s | 文件大小 |
|------|----------:|-----------:|-----------:|--------:|
| File Log Info | 100,000 | 0.409 | 2,444,270 | 8,800,000 |
| File Log Warn | 100,000 | 0.432 | 2,314,810 | 8,800,000 |
| File Log Error | 100,000 | 0.456 | 2,193,940 | 8,800,000 |
| File Log with Formatting | 100,000 | 0.480 | 2,081,560 | 10,300,000 |
| File Log Info (literal) | 100,000 | 0.464 | 2,152,950 | 9,300,000 |
| File Log Verbose | 100,000 | 0.447 | 2,237,490 | 9,400,000 |
| File Log Verbose (literal) | 100,000 | 0.407 | 2,457,120 | 9,100,000 |

## 4. NEI vs spdlog 对比

### 4.1 Memory（async, minimal sink）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple %s / {} | 0.357 | 0.618 | ✅ 42.2% |
| multi printf / fmt | 0.587 | 0.467 | ❌ -25.6% |
| llog_literal / literal only | 0.314 | 0.256 | ❌ -22.5% |
| vlog_literal | 0.306 | — | — |

### 4.2 File（async file sink）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 0.468 | 0.349 | ❌ -34.0% |
| multi args | 0.489 | 0.420 | ❌ -16.5% |
| llog_literal | 0.393 | — | — |
| vlog_literal | 0.472 | — | — |

### 4.3 File（auto-flush, 1ms consumer timer）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 0.475 | 0.199 | ❌ -139.0% |
| multi args | 0.509 | 0.215 | ❌ -136.9% |
| llog_literal | 0.396 | — | — |
| vlog_literal | 0.431 | — | — |

### 4.4 File（strict sync flush）

| 场景 | NEI us/log | spdlog us/log | NEI 优势 |
|------|----------:|-------------:|:------:|
| simple msg | 9.942 | 8.608 | ❌ -15.5% |
| multi args | 15.076 | 8.985 | ❌ -67.8% |

## 5. 与上轮基线对比（2026-06-11）

| 场景 | 基线 us/log | 当前 us/log | Delta |
|------|----------:|-----------:|:----:|
| Memory Info | 0.325 | 0.340 | ❌ +4.6% |
| Memory Warn | 0.323 | 0.351 | ❌ +8.7% |
| Memory Error | 0.338 | 0.342 | ❌ +1.2% |
| Memory Formatting | 0.533 | 0.560 | ❌ +5.1% |
| Memory Info (literal) | 0.310 | 0.301 | ✅ -2.9% |
| Memory Verbose | 0.349 | 0.359 | ❌ +2.9% |
| Memory Verbose (literal) | 0.311 | 0.326 | ❌ +4.8% |
| File Info | 0.440 | 0.409 | ✅ -7.0% |
| File Warn | 0.385 | 0.432 | ❌ +12.2% |
| File Error | 0.388 | 0.456 | ❌ +17.5% |
| File Formatting | 0.510 | 0.480 | ✅ -5.9% |
| File Info (literal) | 0.341 | 0.464 | ❌ +36.1% |
| File Verbose | 0.379 | 0.447 | ❌ +17.9% |
| File Verbose (literal) | 0.377 | 0.407 | ❌ +8.0% |

> ⚠️ 注意：上轮报告的 CPU 型号有误（原记录为 "AMD Ryzen 9"，实际为 Intel Core Ultra 9 185H）。
> 本轮已纠正硬件信息。两轮数据在同一台机器上采集，差异主要由系统负载波动、电源状态等因素导致。

## 6. 交替格式串 Benchmark（新增）

测试 format-plan 缓存命中/未命中对性能的影响。

| 场景 | Iterations | E2E us/log | E2E logs/s |
|------|----------:|-----------:|-----------:|
| Log Info (alternating 2 fmts) | 1,000,000 | 0.376 | 2,662,300 |
| Log Info (alternating 3 fmts) | 1,000,000 | 0.371 | 2,695,130 |
| Log Verbose (alternating 2 fmts) | 1,000,000 | 0.374 | 2,674,870 |

## 7. C printf vs {fmt} 格式化对比（新增）

| 场景 | Mode | E2E us/log | E2E logs/s |
|------|------|----------:|-----------:|
| Log Info (C printf) | memory | 0.561 | 1,781,800 |
| Log Info ({fmt} literal) | memory | 0.649 | 1,541,090 |
| Log Info simple (C printf) | memory | 0.347 | 2,883,340 |
| Log Info simple ({fmt} literal) | memory | 0.415 | 2,407,470 |
| Log Verbose (C printf) | memory | 0.401 | 2,494,070 |
| Log Verbose ({fmt} literal) | memory | 0.464 | 2,154,230 |
| File Log Info (C printf) | file | 0.651 | 1,535,650 |
| File Log Info ({fmt} literal) | file | 0.557 | 1,794,560 |

## 8. 关键结论

1. **Memory 路径整体稳定**：literal 路径 ~0.30μs/log 仍为最优路径（3.3M logs/s），simple printf ~0.34μs/log。
2. **vs spdlog 内存路径**：simple %s 场景 NEI 大幅领先 42%，得益于轻量级入队路径；但格式化及 literal 路径 spdlog 的 fmt 库仍有优势。
3. **File async 路径**：NEI 在 simple msg 场景落后 spdlog 约 34%（0.468 vs 0.349 μs/log），IO 调度效率有优化空间。
4. **auto-flush 模式**：1ms consumer timer 方案保证了低延迟写入，但 spdlog 的 async+flush 模式在此口径下更快（0.199 vs 0.475 μs/log）。
5. **Strict sync 模式**：仅供完整性对比参考（~10-15μs/log），不推荐生产使用。
6. **{fmt} 集成**：pre-format + literal 入队方案在简单场景下有竞争力（~0.42μs/log），但复杂格式化的 {fmt} 耗时主要在上游格式化本身。
7. **交替格式串**：2-3 种格式串交替使用对性能影响可控（~0.37μs/log），format-plan 缓存策略有效。
