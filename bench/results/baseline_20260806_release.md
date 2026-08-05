# NEI 全量 Benchmark 基线报告

**日期**: 2026-08-06
**分支**: `chromium_callback`
**构建**: Visual Studio 2022, `cmake --build --preset windows-vs2022-shared-release`
**机器**: Intel Core Ultra 9 185H (6P+8E+2LPE, 16C/22T), Base 2.3GHz / Turbo ~5.1GHz, 32GB RAM, Windows 11

> 与 2026-07-31 基线 (`baseline_20260731_release.md`) 对比。本次主要变更：
> Chromium-style 堆分配回调重写 + RefCountedThreadSafe + SmallObjectAllocator +
> NEI_PARALLEL_DIAGNOSTICS 统一诊断开关（默认 ON）。

---

## 1. 任务调度 Benchmarks

### 1.1 task_thread_bench — 1M tasks

| 场景 | Post | Total | tasks/s |
|------|-----:|-----:|-------:|
| Standard fast-path | ~285ms | ~290ms | ~3.5 M/s |
| Delayed non-fast-path | 25.6ms | ~43ms | 3.91 M/s |
| Multi 4-thread | 389ms | ~389ms | 2.57 M/s |

> Standard 首次测得 9.11 M/s 为 CPU boost 噪声，重测稳定 ~3.5 M/s。

### 1.2 task_threadpool_bench — 1M tasks (NEI_PARALLEL_DIAGNOSTICS=ON)

| 场景 | Post | Total | tasks/s |
|------|-----:|-----:|-------:|
| Standard fast-path | 279.9ms | ~300ms | 3.57 M/s |
| Delayed | 45.2ms | ~48ms | 2.21 M/s |
| Multi 4-thread | 367.8ms | ~370ms | 2.72 M/s |
| Parallel 1-thread | 638.5ms | ~640ms | 1.57 M/s |
| Parallel 4-thread | 631.9ms | ~635ms | 1.58 M/s |
| SingleThread - Standard | 278.0ms | ~280ms | 3.60 M/s |
| SingleThread - Delayed | 35.7ms | ~45ms | 2.80 M/s |
| SingleThread - Multi | 375.0ms | ~375ms | 2.67 M/s |

### 1.3 task_threadpool_bench — 1M tasks (NEI_PARALLEL_DIAGNOSTICS=OFF)

诊断计数关闭后的投递吞吐（用于量化诊断开销）：

| 场景 | tasks/s (ON) | tasks/s (OFF) | 诊断代价 |
|------|-----:|-----:|---:|
| Standard | 3.57 M/s | 4.56 M/s | -22% |
| Delayed | 2.21 M/s | 2.50 M/s | -12% |
| Multi 4-thread | 2.72 M/s | 3.02 M/s | -10% |
| Parallel 1-thread | 1.57 M/s | 1.59 M/s | -1% |
| Parallel 4-thread | 1.58 M/s | 1.72 M/s | -8% |
| SingleThread - Standard | 3.60 M/s | 3.98 M/s | -10% |
| SingleThread - Delayed | 2.80 M/s | 2.85 M/s | -2% |
| SingleThread - Multi | 2.67 M/s | 3.08 M/s | -13% |

### 1.4 parallel_runner_bench

| Bench | 场景 | w=1 | w=2 | w=4 | w=8 | w=16 |
|-------|------|-----:|-----:|-----:|-----:|-----:|
| 1 | atomic increment | 133.0M/s | 133.6M/s | 129.7M/s | 63.8M/s | 58.2M/s |
| 2 | 100K small tasks | 1.88M/s | 1.85M/s | 2.18M/s | 2.32M/s | 2.36M/s |
| 3 | PostTask throughput | 2.43M/s | 2.41M/s | 2.44M/s | 2.39M/s | 2.42M/s |

> Raw pool baseline: 10.2 µs

### 1.5 post_job_bench

| Bench | 指标 | 值 |
|-------|------|-----|
| 0 | Raw pool baseline | **9.8 µs** |
| 1 | Sequential PostJob+Join (50K) | **203,893 jobs/s** (4.9 µs/job) |
| 2 | PostJob 1 worker inner loop (500K ops) | 106.4 M/s |
| 2 | SeqRunner baseline (500K tasks) | 3.7 M/s |
| 2 | PostJob 8 workers inner loop (500K ops) | 23.1 M/s |
| 3 | w=1 (10M ops/worker) | **260.6 M/s** |
| 3 | w=2 | 468.3 M/s (1.8×) |
| 3 | w=4 | 482.7 M/s (1.9×) |
| 3 | w=8 | 558.8 M/s (2.1×) |
| 3 | w=16 | **828.9 M/s** (3.2×) |

---

## 2. 日志系统 Benchmarks

### 2.1 log_bench — 1M iterations, memory sink

| 测试 | E2E logs/sec |
|------|------------:|
| Log Info | 3.90 M |
| Log Warn | 3.75 M |
| Log Error | 3.88 M |
| Log with Formatting | 1.96 M |
| Log Info (literal) | 3.42 M |
| Log Verbose | 3.84 M |
| Log Verbose (literal) | 3.59 M |
| Log Info (alt 2 fmts) | 3.37 M |
| Log Info (alt 3 fmts) | 3.41 M |
| Log Verbose (alt 2 fmts) | 3.59 M |
| Log Info (C printf) | 1.96 M |
| Log Info ({fmt} literal) | 2.04 M |
| Log Info simple (C printf) | 3.78 M |
| Log Info simple ({fmt} literal) | 2.52 M |
| Log Verbose (C printf) | 2.99 M |
| Log Verbose ({fmt} literal) | 2.31 M |

### 2.1b log_bench — 100K iterations, file sink

| 测试 | E2E logs/sec |
|------|------------:|
| File Log Info | 2.89 M |
| File Log Warn | 2.62 M |
| File Log Error | 2.64 M |
| File Log with Formatting | 2.58 M |
| File Log Verbose | 3.27 M |
| File Log Info (literal) | 3.40 M |
| File Log Verbose (literal) | 3.54 M |
| File Log Info (alt 2 fmts) | 2.69 M |
| File Log Verbose (alt 2 fmts) | 3.27 M |
| File Log Info (C printf) | 1.63 M |
| File Log Info ({fmt} literal) | 2.27 M |

### 2.2 log_bench_compare — NEI vs spdlog (1M memory / 100K file)

| 场景 | NEI | spdlog | NEI 优势 |
|------|-----:|-----:|:---:|
| **内存** simple | **3.96 M** | 2.42 M | **1.64×** |
| 内存 multi printf | 1.99 M | **2.29 M** | spdlog +15% |
| **文件** simple | **2.91 M** | 2.72 M | +7% |
| 文件 multi | **2.63 M** | 2.59 M | +2% |
| 文件 autoflush simple | 3.03 M | **5.68 M** | spdlog +87% |
| 文件 strict simple | 88,978 | **132,594** | spdlog +49% |
| 文件 strict multi | **125,967** | 122,959 | +2% |

---

## 3. 网络 Benchmarks — TCP

### 3.1 tcp_loopback_bench — 1024MB 传输

| 缓冲 | 耗时 | 吞吐 |
|------|-----:|-----:|
| 4 KB | 3.74s | 274 MB/s |
| 8 KB | 1.94s | 529 MB/s |
| 16 KB | 1.04s | 981 MB/s |
| 32 KB | 0.60s | 1,714 MB/s |
| 64 KB | 0.46s | 2,223 MB/s |
| 128 KB | 0.30s | 3,373 MB/s |
| 256 KB | 0.25s | **4,083 MB/s** |
| 512 KB | 0.33s | 3,141 MB/s |
| **1 MB** ★ | 0.27s | 3,786 MB/s |

### 3.2 tcp_throughput_bench — 500MB, 双 IO 线程, FNV-1a 验证

| 缓冲 | 耗时 | 吞吐 |
|------|-----:|-----:|
| 64 KB (默认) | 1.44s | 348 MB/s |
| **512 KB** ★ | 0.62s | **809 MB/s** |

### 3.3 tcp_rtt_bench — 1000 并发连接

| 指标 | 值 |
|------|-----|
| 连接数 | 1000 |
| Workers | 4 |
| RTT min | 4,685 µs |
| RTT avg | 9,860 µs |
| RTT max | 14,182 µs |
| RTT p50 | **13,343 µs** |
| RTT p99 | 14,173 µs |
| 总耗时 | 0.612s |

### 3.4 tcp_conn_stress_bench — 1000 连接压力

| 指标 | 值 |
|------|-----|
| 连接数 | 1000 |
| Workers | 4 |
| 耗时 | 0.609s |
| 速率 | **1,642 conn/s** |
| 服务端失败 | 1 |

---

## 4. 网络 Benchmarks — TLS (mbedTLS)

### 4.1 tls_throughput_bench — 500MB, 单线程, FNV-1a 验证

| 缓冲 | 耗时 | 吞吐 |
|------|-----:|-----:|
| 64 KB (默认) | 4.44s | 112.5 MB/s |
| **16 KB** ★ | 3.59s | **139.3 MB/s** |

---

## 5. 关键结论（vs 2026-07-31 基线）

| 维度 | 变化 |
|------|------|
| **PostTask** | threadpool 投递 +15~75%（主要为 NEI_PARALLEL_DIAGNOSTICS 计数代价，可 OFF 消除；Parallel 1-thread 瓶颈在别处） |
| **post_job** | ≈ 持平，扩展性更好（w=16 3.2× vs 3.0×） |
| **TCP** | loopback 128KB +11%、256KB +30%、throughput 512KB +12%、RTT p50 -30% |
| **TLS** | 16KB 甜点 +6% |
| **日志** | 待补充 |

## 附录: 构建命令备忘

```powershell
# Windows Release
cmake --build --preset windows-vs2022-shared-release --target <target>

# 关闭并行调度诊断（省 ~8-22% 投递吞吐，以正确性/调试为代价）
cmake -S . -B build/windows-vs2022-shared -DNEI_ENABLE_PARALLEL_DIAGNOSTICS=OFF
```
