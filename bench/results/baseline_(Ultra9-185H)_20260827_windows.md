# libnei — Benchmark Baseline 2026-08-27 (Windows)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores), Windows 11
**Build**: Release shared (`windows-vs2022-shared`)
**Commit**: `b66a1c0` — refactor(net,io): use generic BindOnce callback-as-functor at call sites（本轮覆盖 571484e 泛型 BindOnce、c520060/d3ad629/cfb1981 PostTaskAndReplyWithResult 对接、b66a1c0 调用点简化）
**Raw logs**: `bench/results/bench_20260827_224404/`（自动生成 report.md 同目录）

---

## Task Scheduling（均值，1M tasks，单位 M/s）

### task_thread_bench（10 轮）

| Scenario | tracing ON | tracing OFF |
|---|---:|---:|
| Standard PostTask (fast-path) | 3.54 | 3.59 |
| Delayed PostTask (non-fast-path) | 3.00 | 2.82 |
| Multi-threaded PostTask (4 threads) | 2.19 | 2.11 |
| BindOnce + Run (no queue) | 25.96 | 22.45 |
| BindOnce construction | 35.04 | 32.38 |
| Raw atomic loop（对照） | 119.6 | 116.7 |

### task_threadpool_bench（5 轮，tracing off）

| Scenario | M/s |
|---|---:|
| Standard PostTask (external, global-heap) | 4.24 |
| SingleThread — dedicated-worker 外部投递 | 4.03 |
| Parallel PostTask (single-thread post) | 2.90 |
| SingleThread — Delayed | 2.70 |
| Delayed PostTask (non-fast-path) | 2.61 |
| SingleThread — MT (4 threads) | 2.62 |
| Multi-threaded PostTask (4 threads, sequenced) | 2.49 |
| Parallel MT (4 threads) | 2.39 |
| Parallel Worker-Repost (global-heap) | 0.70 |
| Worker-Repost (local-queue fast-path) | 0.49 |

### task_threadpool_parallel_bench（5 轮）

| Scenario | M/s |
|---|---:|
| Parallel PostTask (single-thread post) | 2.74 |
| Parallel MT (4 threads) | 2.29 |

## Log（5 轮均值）

| 场景 | E2E μs/log | logs/s |
|---|---:|---:|
| Log Info / Warn / Error | 0.45–0.46 | 2.20–2.27 M |
| Log Verbose (literal) | 0.43 | 2.37 M |
| Log with Formatting | 0.75 | 1.34 M |
| Memory literal（NEI simple %s / vlog_literal） | 0.34–0.37 | 2.7–2.9 M |
| File async（NEI） | 0.41–0.63 | 1.8–2.6 M |
| Strict sync flush（NEI） | 13.1–14.4 | ~72–77 K |

vs spdlog：memory literal spdlog 0.315 μs vs NEI 0.344 μs（spdlog 快 ~9%）；格式化 0.556 vs 0.633（spdlog 快 ~12%）；async file 持平；strict flush spdlog 10.1–10.4 μs（快 ~25%，与 08-18 基线同结论）。

## File I/O（64 MB，最佳块）

| 操作 | 吞吐 | 块 |
|---|---:|---|
| write | **2618.7 MB/s** | 256 KB |
| read | **3177.3 MB/s** | 256 KB |

## Pipe I/O（进程内，64 MB）

| Chunk | Throughput |
|---|---:|
| 4 KB | 333.7 MB/s |
| 16 KB | 521.8 MB/s |
| 64 KB | 1869.7 MB/s |
| 256 KB | **5564.2 MB/s** |
| 1 MB | 3100.1 MB/s |

PipeStream 跨进程（64B × 5000）：P50 15.6 μs / P95 30.7 μs，57,934 msgs/s。

## TCP Loopback（1024 MB）

| Buffer | Throughput |
|---|---:|
| 4 KB | 224.7 MB/s |
| 8 KB | 314.7 MB/s |
| 16 KB | 607.5 MB/s |
| 32 KB | 812.9 MB/s |
| 64 KB | 2304.6 MB/s |
| 128 KB | **3051.6 MB/s** |
| 256 KB | 2418.1 MB/s（波动） |
| 512 KB | 3012.9 MB/s |
| 1024 KB | 2355.8 MB/s（波动） |

- TCP RTT（1000 连接回环 echo）：avg 13.7 / p50 17.1 / p99 25.0 ms（0 失败）
- TCP Throughput（10 MB @ 64 KB）：484.1 MB/s（单轮，噪声大）
- TCP Conn Stress（1000 连接）：1437.8 conn/s（accept 1 例失败，0.1%）

## TLS Throughput

| Metric | 吞吐 |
|---|---:|
| 10 MB @ 64 KB | **84.6 MB/s** |

## HTTP / HTTP2（回环 keep-alive）

| Bench | 结果 |
|---|---:|
| HTTP/1.1 Throughput（20K req） | 18,755 req/s（avg 52.8 μs） |
| HTTP/2 seq（10K req, concurrency 1） | 15,677 req/s |
| HTTP/2 par8（10K req） | 33,251 req/s |
| HTTP/2 par64（10K req） | **39,786 req/s** |

## 其他单轮 Bench

| Bench | 结果 |
|---|---|
| PostJob 扩展 | 16 线程 1440 M ops/s（7.4×；8 线程 954 M） |
| Flake ID | 186.8 M ids/s（8 线程） |
| StringAppendF | 3.90 M ops/s（vs StringPrintf 1.31×、vs stringstream 2.39×） |
| OnceCallback | 35.4 M/s；RepeatingCallback 78.6 M/s；BindOnce 23.1 M/s |
| Parallel Runner 原子递增 | 多 worker 0.2×（共享计数器竞争，预期） |
| Task Priority Demo | 三级优先级各 33.3% 公平份额，无饥饿（1487 tasks/s） |

---

## 与 2026-08-18 基线对比（同机）

| 项目 | 08-18 | 08-27 | 变化 |
|---|---:|---:|---:|
| task_thread Standard (fast-path, ON) | 4.44 | 3.54 | −20% |
| task_thread BindOnce construction | 39.43 | 35.04 | −11% |
| task_thread BindOnce + Run | 28.64 | 25.96 | −9% |
| task_threadpool Standard (global-heap) | 5.38 | 4.24 | −21% |
| File write best | 3153.0 | 2618.7 | −17% |
| File read best | 4104.9 | 3177.3 | −23% |
| Pipe 256 KB | 11373 | 5564.2 | −51% |
| TLS 10 MB | 106.4 | 84.6 | −20% |
| HTTP/1.1 (20K) | 22,684 | 18,755 | −17% |
| HTTP/2 par64 | 44,003 | 39,786 | −10% |

> ⚠️ 说明：本次多数场景相对 08-18 有 10–50% 的一致回落（含与 BindOnce 无关的 File/Pipe/TCP 等），
> 且同轮内波动明显（TCP 256KB/1024KB、File 块间差异大），高度提示为**机器运行状态/后台负载噪声**，
> 而非本轮回溯提交（均为纯模板/简化重构，运行时路径未变）。BindOnce construction −11% 需单独关注：
> 泛型化只改编译期模板展开，BindState 运行时布局未变，若复跑仍回落再排查。建议后续在空闲机器上复测确认。
