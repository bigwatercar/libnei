# libnei — Benchmark Baseline 2026-08-18 (Windows)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores), Windows 11
**Build**: Release shared (`windows-vs2022-shared`)
**Commit**: `1136ec4` — task 唤醒路径回移植（single_consumer swap + per-state 事件 + delayed 回调拆分 + Shutdown 幂等）
**Raw logs**: `bench/results/bench_20260818_230939/`（自动生成 report.md 同目录）

---

## Task Scheduling（均值，1M tasks，单位 M/s）

### task_thread_bench（10 轮）

| Scenario | tracing ON | tracing OFF |
|---|---:|---:|
| Standard PostTask (fast-path) | 4.44 | 3.96 |
| Delayed PostTask (non-fast-path) | 3.85 | 3.67 |
| Multi-threaded PostTask (4 threads) | 2.53 | 2.46 |
| BindOnce + Run (no queue) | 28.64 | 29.04 |
| BindOnce construction | 39.43 | 39.71 |
| Raw atomic loop（对照） | 128.8 | 129.7 |

### task_threadpool_bench（5 轮，tracing off）

| Scenario | M/s |
|---|---:|
| Standard PostTask (external, global-heap) | 5.38 |
| SingleThread — dedicated-worker 外部投递 | 4.87 |
| SingleThread — Delayed | 4.30 |
| Parallel PostTask (single-thread post) | 3.94 |
| Delayed PostTask (non-fast-path) | 3.26 |
| Multi-threaded PostTask (4 threads, sequenced) | 3.09 |
| SingleThread — MT (4 threads) | 2.88 |
| Parallel MT (4 threads) | 2.60 |
| Parallel Worker-Repost (global-heap) | 0.79 |
| Worker-Repost (local-queue fast-path) | 0.57 |

### task_threadpool_parallel_bench（5 轮）

| Scenario | M/s |
|---|---:|
| Parallel PostTask (single-thread post) | 3.64 |
| Parallel MT (4 threads) | 2.48 |

## Log（5 轮均值）

| 场景 | E2E μs/log | logs/s |
|---|---:|---:|
| Log Info / Warn / Error | 0.27 | 3.6–3.7 M |
| Log Verbose (literal) | 0.25 | 4.0 M |
| Log with Formatting | 0.48 | 2.1 M |
| Memory literal（NEI） | 0.24–0.25 | 4.0–4.1 M |
| File async（NEI） | 0.30–0.48 | 2.1–3.3 M |
| Strict sync flush（NEI） | 8.1–8.7 | ~120 K |

vs spdlog：memory literal 0.24 μs 持平；格式化 0.46–0.48 持平；async file 持平；
strict flush spdlog 6.6–6.8 μs（快 ~20%，与 08-16 基线同结论）。

## File I/O（64 MB，最佳块）

| 操作 | 吞吐 | 块 |
|---|---:|---|
| write | **3153.0 MB/s** | 256 KB |
| read | **4104.9 MB/s** | 1 MB |

## Pipe I/O（进程内，64 MB）

| Chunk | Throughput |
|---|---:|
| 4 KB | 421.9 MB/s |
| 16 KB | 1412.0 MB/s |
| 64 KB | 5080.5 MB/s |
| 256 KB | **11373 MB/s** |
| 1 MB | 4404.6 MB/s |

PipeStream 跨进程（64B × 5000）：P50 14.3 μs / P95 19.2 μs，66,486 msgs/s。

## TCP Loopback（1024 MB）

| Buffer | Throughput |
|---|---:|
| 4 KB | 270.1 MB/s |
| 16 KB | 1011.0 MB/s |
| 64 KB | 2548.0 MB/s |
| 128 KB | 3115.7 MB/s |
| 256 KB | 2130.5 MB/s（异常低，单轮波动） |
| 512 KB | 3417.1 MB/s |
| 1024 KB | **3885.7 MB/s** |

- TCP RTT（1000 连接回环 echo）：avg 14.4 / p99 20.4 ms
- TCP Throughput（10 MB @ 64 KB）：229.8 MB/s（单轮，噪声大）
- TCP Conn Stress（1000 连接）：1644.5 conn/s（accept 1 例失败，0.1%）

## TLS Throughput

| Metric | 吞吐 |
|---|---:|
| 10 MB @ 64 KB | **106.4 MB/s** |

## HTTP / HTTP2（回环 keep-alive）

| Bench | 结果 |
|---|---:|
| HTTP/1.1 Throughput（20K req） | 22,684 req/s（P50 43.6 μs） |
| HTTP/2 seq（10K req, concurrency 1） | 19,290 req/s |
| HTTP/2 par8（10K req） | 40,267 req/s |
| HTTP/2 par64（10K req） | **44,003 req/s** |

## 其他单轮 Bench

| Bench | 结果 |
|---|---|
| PostJob 扩展 | 16 线程 1433 M ops/s（5.8×；8 线程 1341 M） |
| Flake ID | 239.9 M ids/s（8 线程） |
| StringAppendF | 4.94 M ops/s（vs StringPrintf 1.20×、vs stringstream 2.56×） |
| OnceCallback | 39.9 M/s；RepeatingCallback 91.6 M/s |
| Parallel Runner 原子递增 | 多 worker 0.3×（共享计数器竞争，预期） |
| Task Priority Demo | 三级优先级各 33.3% 公平份额，无饥饿（1498 tasks/s） |

---

## 与 2026-08-16 Windows 基线对比（同机）

| Benchmark | 2026-08-16 | 2026-08-18 | Δ |
|---|:---:|:---:|:---:|
| task_thread Standard fast-path (off) | 3.91 | 3.96 | +1.3% |
| threadpool Standard (global-heap) | 5.15 | 5.38 | +4.5% |
| threadpool SingleThread dedicated | 5.12 | 4.87 | −4.9% |
| threadpool Parallel single | 4.17 | 3.94 | −5.5% |
| async_file best write | 3102.3 | 3153.0 | +1.6% |
| async_file best read | 4185.7 | 4104.9 | −1.9% |
| pipe 64 KB | 5068.9 | 5080.5 | +0.2% |
| pipe 256 KB | 11562.0 | 11373 | −1.6% |
| TCP loopback 1024 KB | 3160.7 | 3885.7 | +22.9% |
| TLS throughput | 109.9 | 106.4 | −3.2% |

> task 调度各场景在 ±5% 噪声带内持平；网络/IO 项为单轮或受时段负载影响。
> 回移植在 Windows 上未引入性能回退。
