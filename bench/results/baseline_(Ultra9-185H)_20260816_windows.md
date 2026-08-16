# libnei — Benchmark Baseline 2026-08-16

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores), Windows 11
**Build**: Release shared (`windows-vs2022-shared`)
**Commit**: `71788de` — includes HttpRequestHandle, TSan race fixes, TLS/HTTP2 bench Mbed TLS threading fix
**Raw logs**: `bench/results/bench_20260816_192723/` (auto-generated report.md alongside)

---

## Task Scheduling（10 轮均值，1M tasks，单位 M/s）

### task_thread_bench（tracing on / off）

| Scenario | tracing ON | tracing OFF |
|---|---:|---:|
| Standard PostTask (fast-path) | 3.86 | 3.91 |
| Delayed PostTask (non-fast-path) | 4.29 | 4.19 |
| Multi-threaded PostTask (4 threads) | 2.52 | 2.47 |
| BindOnce + Run (no queue) | 30.57 | 30.33 |
| BindOnce construction | 43.00 | 41.06 |
| Raw atomic loop（对照） | 135.0 | 130.9 |

### task_threadpool_bench（5 轮均值，1M tasks，tracing off）

| Scenario | M/s |
|---|---:|
| Standard PostTask (external, global-heap) | 5.15 |
| SingleThread — dedicated-worker 外部投递 | 5.12 |
| Parallel PostTask (single-thread post) | 4.17 |
| Delayed PostTask (non-fast-path) | 3.43 |
| Multi-threaded PostTask (4 threads, sequenced) | 3.02 |
| Parallel Worker-Repost (global-heap) | 0.69 |
| Worker-Repost (local-queue fast-path) | 0.50 |

## Log（5 轮均值）

| 场景 | E2E μs/log | logs/s |
|---|---:|---:|
| Log Info / Warn / Error | 0.27 | 3.7–3.8 M |
| Log Verbose (literal) | 0.25 | 4.0 M |
| Log with Formatting | 0.46 | 2.2 M |
| File sink multi | 0.35 | 2.8 M |
| File sink literal | 0.23–0.25 | 4.0–4.4 M |
| Strict sync flush | 8.1–8.7 | ~123 K |

vs spdlog：literal 0.23 μs（NEI 0.24–0.25）；格式化 0.46–0.48 μs 持平；async file 持平；strict flush 6.6 μs（spdlog 更快 ~20%）。

## File I/O（64 MB，最佳块）

| 操作 | 吞吐 | 块 |
|---|---:|---|
| write | **3102.3 MB/s** | 256 KB |
| read | **4185.7 MB/s** | 4 MB |

## Pipe I/O（进程内，64 MB）

| Chunk | Throughput |
|---|---:|
| 4 KB | 437.3 MB/s |
| 16 KB | 1651.5 MB/s |
| 64 KB | 5068.9 MB/s |
| 256 KB | **11562.0 MB/s** |
| 1 MB | 5372.2 MB/s |

PipeStream 跨进程（64B × 5000）：P50 13.7 μs / P95 18.7 μs，68,965 msgs/s。

## TCP Loopback（1024 MB）

| Buffer | Throughput |
|---|---:|
| 4 KB | 271.3 MB/s |
| 16 KB | 949.8 MB/s |
| 64 KB | 2150.4 MB/s |
| 128 KB | 2928.3 MB/s |
| 256 KB | **3463.0 MB/s** |
| 512 KB | 3181.9 MB/s |
| 1024 KB | 3160.7 MB/s |

- TCP RTT（1000 连接回环 echo）：min 4.2 / avg 10.1 / p99 14.1 ms
- TCP Throughput（10 MB @ 64 KB）：604.8 MB/s（单轮噪音大，185–609 波动）
- TCP Conn Stress（1000 连接）：1647 conn/s（accept 1 例偶发失败，0.1%，未复现）

## TLS Throughput

| Metric | 吞吐 |
|---|---:|
| 10 MB @ 64 KB | **109.9 MB/s** |

> 注：TLS bench 此前因 Mbed TLS 线程回调未注册而静默失败（`rsa_gen_key failed`），
> 已在 `71788de` 修复并重新纳入本基线。

## 其他单轮 Bench

| Bench | 结果 |
|---|---|
| PostJob 扩展 | 16 线程 460 M ops/s（多核竞争后约 1.8×，1→2 线程近线性 2.0×） |
| Flake ID | 277.5 M ids/s（8 线程） |
| StringAppendF | 4.52 M ops/s（vs stringstream 2.29×） |
| OnceCallback | 41.7 M/s；RepeatingCallback 96.9 M/s |
| Parallel Runner 原子递增 | 多 worker 0.3–0.4×（共享计数器竞争，预期） |
| Task Priority Demo | 三级优先级各 33.3% 公平份额，无饥饿（吞吐 1498 tasks/s） |

---

## 与 2026-08-11 基线对比（Windows，同机）

| Benchmark | 2026-08-11 | 2026-08-16 | Δ |
|---|:---:|:---:|:---:|
| async_file best write | 3145.4 | 3102.3 | −1.4% |
| async_file best read | 3925.9 | 4185.7 | +6.6% |
| pipe 16 KB | 619.0 | 1651.5 | +166.8% |
| pipe 64 KB | 2254.8 | 5068.9 | +124.8% |
| pipe 256 KB | 6052.0 | 11562.0 | +91.0% |
| TCP loopback 256 KB | 3918.2 | 3463.0 | −11.6% |
| TCP loopback 1024 KB | 4144.0 | 3160.7 | −23.7% |
| TCP throughput 10 MB@64 KB | 559.8 | 604.8* | +8.0%* |
| TLS throughput 10 MB@64 KB | 96.0 | 109.9 | +14.5% |

\* 单轮测量，噪音大（185–609 波动），参考性有限。

## Notes

- **Pipe I/O** 16 KB+ 档较旧基线大幅提升（+91%~+167%），归因于期间 PipeStream
  写队列重构与 IO 线程迁移；4 KB 档持平（小包开销主导）。
- **TCP loopback** 大块较旧基线回落 ~12–24%，同轮数据在 3.2–3.5 GB/s 平台，
  可能与当日机器状态或 1024 MB 数据量下单轮噪音有关；TCP throughput 独立
  bench（10 MB）反而 +8%。建议下次多轮取中位确认。
- **Task 调度**：本次为场景化口径（fast-path 3.9 M/s ≈ 256 ns/task），旧基线
  417 ns/task 口径未对齐，不直接对比。
- **TLS**：修复证书生成后 +14.5%，与 mbedTLS 软件加密瓶颈（CPU 频率相关）
  一致。
