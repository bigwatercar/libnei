# NEI 全量 Benchmark 基线报告

**日期**: 2026-08-09
**分支**: `dev` @ HEAD `7edd09e`
**构建**: Visual Studio 2022, `cmake --build --preset windows-vs2022-shared` (Release x64, shared)
**机器**: Intel Core Ultra 9 185H (6P+8E+2LPE, 16C/22T), Base 2.3GHz / Turbo ~5.1GHz, 32GB RAM, Windows 11
**诊断开关**: `NEI_PARALLEL_DIAGNOSTICS=0`, `NEI_ALLOCATOR_DIAGNOSTICS=0`（均默认 OFF）
**任务调度口径**: 1M tasks, tracing=off

> 相对 08-06 基线（`baseline_(Ultra9-185H)_20260806_windows.md`, chromium_callback 分支）的主要变更：
> 统一堆调度器、Chromium 对齐并行聚合（`d134149`，ParallelTaskSequence 单任务取模型）、
> dedicated worker 唤醒修复（`0e3a297`/`db41005`）、post_job_bench UAF 修复（`7edd09e`）。

---

## 1. 任务调度 Benchmarks

### 1.1 task_thread_bench — 1M tasks, tracing=off

| 场景 | Post | Total | tasks/s |
|------|-----:|-----:|-------:|
| Raw atomic loop（基线） | 8.1ms | 8.1ms | 123.8 M/s |
| BindOnce 构造（基线） | 23.3ms | 23.3ms | 43.0 M/s |
| BindOnce + Run（基线） | 32.4ms | 32.4ms | 30.8 M/s |
| Standard fast-path | 235.5ms | 235.5ms | 4.25 M/s |
| Delayed non-fast-path | 25.5ms | 39.0ms | 3.92 M/s (post) / 2.57 M/s (total) |
| Multi 4-thread | 391.5ms | 391.7ms | 2.55 M/s |

### 1.2 task_threadpool_bench — 1M tasks, tracing=off, NEI_PARALLEL_DIAGNOSTICS=ON

| 场景 | Post | Total | tasks/s |
|------|-----:|-----:|-------:|
| Standard (external, global-heap) | 196.8ms | 216.8ms | 4.61 M/s |
| Worker-Repost (local-queue fast-path) | 2261.7ms | 2261.7ms | 0.44 M/s |
| Delayed (non-fast-path, 100K) | 27.3ms | 41.1ms | 2.43 M/s |
| Multi 4-thread (sequenced) | 315.8ms | 316.0ms | 3.16 M/s |
| Parallel (single-thread post) | 216.5ms | 1482.3ms | 0.67 M/s |
| Parallel Worker-Repost (global-heap) | 1997.9ms | 1997.9ms | 0.50 M/s |
| Parallel Multi 4-thread | 397.0ms | 1655.4ms | 0.60 M/s |
| SingleThread · Standard | 208.9ms | 214.4ms | 4.66 M/s |
| SingleThread · Delayed (100K) | 27.3ms | 37.5ms | 2.67 M/s |
| SingleThread · Multi 4-thread | 322.6ms | 323.4ms | 3.09 M/s |

### 1.3 task_threadpool_bench — 1M tasks, tracing=off, NEI_PARALLEL_DIAGNOSTICS=OFF

诊断计数关闭后的投递吞吐（用于量化诊断开销；负百分比 = 计数 ON 更慢）：

| 场景 | tasks/s (ON) | tasks/s (OFF) | 诊断代价 |
|------|-----:|-----:|---:|
| Standard | 4.61 | 4.86 | -5% |
| Worker-Repost | 0.44 | 0.40 | -10% |
| Delayed | 2.43 | 2.00 | -21% |
| Multi 4-thread | 3.16 | 2.89 | -9% |
| Parallel 1-thread | 0.67 | 0.73 | +8% |
| Parallel Worker-Repost | 0.50 | 0.55 | +9% |
| Parallel 4-thread | 0.60 | 0.59 | -2% |
| SingleThread · Standard | 4.66 | 5.01 | +7% |
| SingleThread · Delayed | 2.67 | 2.18 | -22% |
| SingleThread · Multi | 3.09 | 2.85 | -8% |

> ON/OFF 差异含运行噪声（±10% 内），诊断计数本身开销有限。

### 1.4 parallel_runner_bench

| Bench | w=1 | w=2 | w=4 | w=8 | w=16 |
|-------|-----:|-----:|-----:|-----:|-----:|
| 1 · atomic increment | 135.1M/s | 71.4M/s | 65.4M/s | 62.6M/s | 64.0M/s |
| 2 · 100K small tasks | 795K/s | 679K/s | 652K/s | 687K/s | 621K/s |
| 3 · PostTask throughput | 530K/s | 533K/s | 462K/s | 449K/s | 438K/s |

> Raw pool baseline: 9.9 µs

### 1.5 post_job_bench

| Bench | 指标 | 值 |
|-------|------|-----|
| 0 | Raw pool baseline | **9.2 µs** |
| 1 | Sequential PostJob+Join (50K) | **290,020 jobs/s** (3.4 µs/job) |
| 2 | PostJob 1 worker inner loop (500K) | **119.7 M/s** |
| 2 | SeqRunner (500K tasks) | 4.4 M/s |
| 2 | PostJob 8 workers inner loop (500K) | **35.4 M/s** |
| 3 | w=1 (10M ops/worker) | **263.4 M/s** |
| 3 | w=2 | 487.8 M/s (1.9×) |
| 3 | w=4 | 431.0 M/s (1.6×) |
| 3 | w=8 | 432.3 M/s (1.6×) |
| 3 | w=16 | **502.6 M/s** (1.9×) |

> 注：Bench 3 原先因 `[&]` 捕获栈 `ctrs` 的 UAF 在 Windows 崩溃（0xC0000005），已改为 heap 捕获（commit `7edd09e`）。

---

## 2. 日志系统 Benchmarks

### 2.1 log_bench — 1M iterations, memory sink

| 测试 | E2E logs/sec |
|------|------------:|
| Log Info | 3.60 M |
| Log Warn | 3.43 M |
| Log Error | 3.90 M |
| Log with Formatting | 2.04 M |
| Log Info (literal) | 3.80 M |
| Log Verbose | 3.64 M |
| Log Verbose (literal) | 3.56 M |
| Log Info (alt 2 fmts) | 3.43 M |
| Log Info (alt 3 fmts) | 3.51 M |
| Log Verbose (alt 2 fmts) | 3.41 M |
| Log Info (C printf) | 1.99 M |
| Log Info ({fmt} literal) | 1.91 M |
| Log Info simple (C printf) | 3.78 M |
| Log Info simple ({fmt} literal) | 2.52 M |
| Log Verbose (C printf) | 3.17 M |
| Log Verbose ({fmt} literal) | 2.38 M |

### 2.1b log_bench — 100K iterations, file sink

| 测试 | E2E logs/sec |
|------|------------:|
| File Log Info | 2.91 M |
| File Log Warn | 2.82 M |
| File Log Error | 2.86 M |
| File Log with Formatting | 2.53 M |
| File Log Verbose | 2.99 M |
| File Log Info (literal) | 3.53 M |
| File Log Verbose (literal) | 3.88 M |
| File Log Info (alt 2 fmts) | 3.03 M |
| File Log Verbose (alt 2 fmts) | 3.28 M |
| File Log Info (C printf) | 1.73 M |
| File Log Info ({fmt} literal) | 2.30 M |

### 2.2 log_bench_compare — NEI vs spdlog (1M memory / 100K file / 10K flush)

| 场景 | NEI | spdlog | NEI 优势 |
|------|-----:|-----:|:---:|
| **内存** simple | **4.005 M** | 2.289 M | **1.75×** |
| 内存 multi printf | 2.010 M | **2.200 M** | spdlog +9% |
| 内存 {fmt} literal multi | 1.978 M | — | — |
| **文件** simple | 2.778 M | **3.146 M** | spdlog +13% |
| 文件 multi | 2.539 M | 2.567 M | ≈ |
| 文件 {fmt} literal multi | 2.150 M | — | — |
| 文件 autoflush simple | 2.689 M | **5.643 M** | spdlog +110% |
| 文件 autoflush multi | 2.660 M | **4.235 M** | spdlog +59% |
| 文件 strict simple | 76,740 | **128,472** | spdlog +67% |
| 文件 strict multi | 82,612 | **125,080** | spdlog +51% |

---

## 3. 网络 Benchmarks — TCP

### 3.1 tcp_loopback_bench — 1024MB 传输

| 缓冲 | 耗时 | 吞吐 |
|------|-----:|-----:|
| 4 KB | 3.54s | 289 MB/s |
| 8 KB | 1.84s | 556 MB/s |
| 16 KB | 1.04s | 987 MB/s |
| 32 KB | 0.75s | 1,358 MB/s |
| 64 KB | 0.43s | 2,365 MB/s |
| 128 KB | 0.27s | **3,760 MB/s** |
| 256 KB | 0.40s | 2,593 MB/s |
| 512 KB | 0.32s | 3,221 MB/s |
| **1 MB** ★ | 0.32s | 3,215 MB/s |

### 3.2 tcp_throughput_bench — 500MB, 双 IO 线程, FNV-1a 验证

| 缓冲 | 耗时 | 吞吐 |
|------|-----:|-----:|
| 64 KB (默认) | — | 533 MB/s |
| **512 KB** ★ | — | **723 MB/s** |

### 3.3 tcp_rtt_bench — 1000 并发连接

| 指标 | 值 |
|------|-----|
| 连接数 | 1000 |
| Workers | 4 |
| RTT min | 5,039 µs |
| RTT avg | 10,034 µs |
| RTT max | 13,736 µs |
| RTT p50 | **12,536 µs** |
| RTT p90 | 13,619 µs |
| RTT p99 | 13,734 µs |
| 总耗时 | 0.612s |

### 3.4 tcp_conn_stress_bench — 1000 连接压力

| 指标 | 值 |
|------|-----|
| 连接数 | 1000 |
| Workers | 4 |
| 耗时 | 0.608s |
| 速率 | **1,645 conn/s** |
| 服务端失败 | 1 |

---

## 4. 网络 Benchmarks — TLS (mbedTLS)

### 4.1 tls_throughput_bench — 500MB, 单线程, FNV-1a 验证

| 缓冲 | 耗时 | 吞吐 |
|------|-----:|-----:|
| 64 KB (默认) | — | 117.5 MB/s |
| **16 KB** ★ | — | **141.4 MB/s** |

---

## 5. 关键结论（vs 2026-08-06 基线）

| 维度 | 变化 |
|------|------|
| **非并行 PostTask** | Standard +7%、SingleThread·Standard +26%、thread Standard +21% —— 均占优 |
| **post_job** | Sequential +42%、1w inner +13%、8w inner +53%；但 scaling 1→16 仅 1.9×（08-06 3.2×），WSL 下亦饱和较早 |
| **Parallel 场景** | **threadpool Parallel −54%~−66%、parallel_runner PostTask −78%**（chromium_callback 的批量实现更快，但非 Chromium 对齐；当前为聚合单任务取模型 + source 锁竞争） |
| **日志** | 与 08-06 基本持平（±10%）；NEI 内存 simple 1.75× spdlog，spdlog 在 autoflush/strict 领先 |
| **TCP** | loopback 128KB +11%、throughput 64KB +53%（512KB −11%）、RTT p50 −6% |
| **TLS** | 64KB +4%、16KB +2% |
