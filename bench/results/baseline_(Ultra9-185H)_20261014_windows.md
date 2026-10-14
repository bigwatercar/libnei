# libnei — Benchmark Baseline 2026-10-14 (Windows)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores), Windows 11
**Build**: Release shared (`windows-vs2022-shared`)
**Commit**: `6031c9b` — build(version): bump to 0.9.1 for release（v0.9.1）
**采集条件**: 机器空闲（采样 CPU 5–13%），无 WinRAR 等后台高负载任务；用于复测 08-27 基线的回落是否为负载噪声。

---

## Task Scheduling（均值，1M tasks，单位 M/s）

### task_thread_bench（10 轮）

| Scenario | tracing ON | tracing OFF |
|---|---:|---:|
| Standard PostTask (fast-path) | 5.37 | 4.30 |
| Delayed PostTask (non-fast-path) | 3.98 | 3.65 |
| Multi-threaded PostTask (4 threads) | 2.54 | 2.68 |
| BindOnce + Run (no queue) | 29.83 | 29.52 |
| BindOnce construction | 40.96 | 39.86 |
| Raw atomic loop（对照） | 131.7 | 132.5 |

### task_threadpool_bench（5 轮，tracing off）

| Scenario | M/s |
|---|---:|
| Standard PostTask (external, global-heap) | 5.25 |
| SingleThread — dedicated-worker 外部投递 | 5.05 |
| Parallel PostTask (single-thread post) | 4.32 |
| SingleThread — Delayed | 4.00 |
| Delayed PostTask (non-fast-path) | 3.48 |
| Multi-threaded PostTask (4 threads, sequenced) | 3.16 |
| SingleThread — MT (4 threads) | 3.04 |
| Parallel MT (4 threads) | 2.61 |
| Parallel Worker-Repost (global-heap) | 0.75 |
| Worker-Repost (local-queue fast-path) | 0.56 |

### task_threadpool_parallel_bench（5 轮）

| Scenario | M/s |
|---|---:|
| Parallel PostTask (single-thread post) | 3.85 |
| Parallel MT (4 threads) | 2.66 |

## Log（5 轮均值）

| 场景 | E2E μs/log | logs/s |
|---|---:|---:|
| Log Info / Warn / Error | 0.39–0.40 | 2.54–2.56 M |
| Log Verbose (literal) | 0.35 | 2.88 M |
| Log Info (literal) | 0.33 | 3.00 M |
| Log with Formatting | 0.73 | 1.37 M |
| Memory literal（NEI llog / vlog_literal） | 0.30–0.32 | 3.2–3.4 M |
| Memory simple %s（NEI） | 0.40 | 2.54 M |
| File async（NEI） | 0.38–0.50 | 2.0–2.6 M |
| Strict sync flush（NEI） | 11.2 | ~90 K |

vs spdlog：memory literal 0.246 vs NEI 0.316（spdlog 快 ~22%）；simple 0.502 vs 0.395（NEI 快 ~21%）；multi 0.524 vs 0.713（spdlog 快 ~27%）；file async 0.35–0.38 vs 0.46–0.50（spdlog 快 ~25%）；strict flush spdlog 9.0–9.7 vs NEI 11.2（spdlog 快 ~15–20%）。

## File I/O（64 MB，最佳块）

| 操作 | 吞吐 | 块 |
|---|---:|---|
| write | **2981.3 MB/s** | 256 KB |
| read | **4025.9 MB/s** | 4 MB |

## Pipe I/O（进程内，64 MB）

| Chunk | Throughput |
|---|---:|
| 4 KB | 411.9 MB/s |
| 16 KB | 1551.4 MB/s |
| 64 KB | 4825.4 MB/s |
| 256 KB | **11074 MB/s** |
| 1 MB | 5104.8 MB/s |

PipeStream 跨进程（64B × 5000）：P50 14.6 μs / P95 18.4 μs，65,935 msgs/s。

## TCP Loopback（1024 MB）

| Buffer | Throughput |
|---|---:|
| 4 KB | 276.0 MB/s |
| 8 KB | 553.3 MB/s |
| 16 KB | 1062.6 MB/s |
| 32 KB | 1319.2 MB/s |
| 64 KB | 1987.5 MB/s |
| 128 KB | 2806.2 MB/s |
| 256 KB | **3595.3 MB/s** |
| 512 KB | 2494.8 MB/s |
| 1024 KB | 2363.9 MB/s |

- TCP RTT（1000 连接回环 echo）：avg 10.7 / p50 15.8 / p99 18.9 ms（0 失败）
- TCP Throughput（10 MB @ 64 KB）：548.0 MB/s（单轮，噪声大）
- TCP Conn Stress（1000 连接）：1403.5 conn/s（accept 1 例失败，0.1%，与 08-27 一致）

## TLS Throughput

| Metric | 吞吐 |
|---|---:|
| 10 MB @ 64 KB | **100.9 MB/s** |

## HTTP / HTTP2（回环 keep-alive）

| Bench | 结果 |
|---|---:|
| HTTP/1.1 Throughput（20K req） | 24,449 req/s（avg 40.4 μs） |
| HTTP/2 seq（10K req, concurrency 1） | 20,899 req/s |
| HTTP/2 par8（10K req） | 42,764 req/s |
| HTTP/2 par64（10K req） | **49,582 req/s** |

## 其他单轮 Bench

| Bench | 结果 |
|---|---|
| PostJob 扩展 | 16 线程 1441 M ops/s（5.8×；8 线程 775 M） |
| Flake ID | 280.0 M ids/s（8 线程） |
| StringAppendF | 5.02 M ops/s（vs StringPrintf 1.18×、vs stringstream 2.74×） |
| OnceCallback | 39.3 M/s；RepeatingCallback 85.1 M/s；BindOnce 38.3 M/s |
| Parallel Runner 原子递增 | 多 worker 0.3×（共享计数器竞争，预期） |
| Task Priority Demo | 三级优先级各 33.3% 公平份额，无饥饿（1498 tasks/s） |

---

## 与 08-18 / 08-27 基线对比（同机）

| 项目 | 08-18 | 08-27 | 10-14 | 10-14 vs 08-27 | 10-14 vs 08-18 |
|---|---:|---:|---:|---:|---:|
| task_thread Standard (fast-path, ON) | 4.44 | 3.54 | 5.37 | +52% | +21% |
| task_thread Delayed (ON) | 3.85 | 3.00 | 3.98 | +33% | +3% |
| task_thread MT 4t (ON) | 2.53 | 2.19 | 2.54 | +16% | +0% |
| task_thread BindOnce + Run | 28.64 | 25.96 | 29.83 | +15% | +4% |
| task_thread BindOnce construction | 39.43 | 35.04 | 40.96 | +17% | +4% |
| task_threadpool Standard (global-heap) | 5.38 | 4.24 | 5.25 | +24% | −2% |
| task_threadpool Parallel PostTask | 3.94 | 2.90 | 4.32 | +49% | +10% |
| task_threadpool Worker-Repost (local) | 0.57 | 0.49 | 0.56 | +14% | −2% |
| File write best (MB/s) | 3153.0 | 2618.7 | 2981.3 | +14% | −5% |
| File read best (MB/s) | 4104.9 | 3177.3 | 4025.9 | +27% | −2% |
| Pipe 256 KB (MB/s) | 11373 | 5564.2 | 11074 | +99% | −3% |
| TLS 10 MB (MB/s) | 106.4 | 84.6 | 100.9 | +19% | −5% |
| HTTP/1.1 (20K, req/s) | 22,684 | 18,755 | 24,449 | +30% | +8% |
| HTTP/2 par64 (req/s) | 44,003 | 39,786 | 49,582 | +25% | +13% |

## 结论

- **08-27 的回落已确认为机器负载噪声**：空闲机器上 10-14 全面回升，多数场景与 08-18 持平或更优（±5% 内为波动）。
- **BindOnce construction 已恢复**（35.04 → 40.96 M/s，高于 08-18 的 39.43）：泛型 BindOnce 重构无运行时回归，关闭该复核项。
- 仍偏低的项（−5% 左右：File write、TLS）处于单轮波动范围，且 08-18 同类项本身即有块间/轮间波动，暂不跟进。
- 本轮 task_thread Standard (tracing ON) 5.37 > OFF 4.30 属 1M 单机波动（ON 轮间 stddev 0.57），不代表 tracing 加速。
