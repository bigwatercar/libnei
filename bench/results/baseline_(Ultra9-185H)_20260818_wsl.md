# libnei — Benchmark Baseline 2026-08-18 (WSL2)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores), WSL2 Ubuntu-24.04 (GCC Release shared)
**Build**: Release shared (`linux-gcc-release-shared`)
**Commit**: `1136ec4` — task 唤醒路径回移植（single_consumer swap + per-state 事件 + delayed 回调拆分 + Shutdown 幂等）
**Raw logs**: `bench/results/wsl_20260818_231246/`（自动生成 report.md 同目录）

---

## Task Scheduling（均值，1M tasks，单位 M/s）

### task_thread_bench（10 轮）

| Scenario | tracing ON | tracing OFF |
|---|---:|---:|
| Standard PostTask (fast-path) | 3.60 | 3.74 |
| Delayed PostTask (non-fast-path) | 3.79 | 4.19 |
| Multi-threaded PostTask (4 threads) | 2.99 | 3.15 |
| BindOnce + Run (no queue) | 28.82 | 29.95 |
| BindOnce construction | 38.21 | 37.91 |
| Raw atomic loop（对照） | 128.5 | 132.2 |

### task_threadpool_bench（5 轮，tracing off）

| Scenario | M/s |
|---|---:|
| Parallel PostTask (single-thread post) | 4.34 |
| SingleThread — dedicated-worker 外部投递 | 3.98 |
| Standard PostTask (external, global-heap) | 3.66 |
| SingleThread — Delayed | 2.95 |
| SingleThread — MT (4 threads) | 2.54 |
| Multi-threaded PostTask (4 threads, sequenced) | 2.34 |
| Delayed PostTask (non-fast-path) | 2.27 |
| Parallel MT (4 threads) | 1.91 |
| Parallel Worker-Repost (global-heap) | 0.42 |
| Worker-Repost (local-queue fast-path) | 0.29 |

### task_threadpool_parallel_bench（5 轮）

| Scenario | M/s |
|---|---:|
| Parallel PostTask (single-thread post) | 4.26 |
| Parallel MT (4 threads) | 1.86 |

## Log（5 轮均值）

| 场景 | E2E μs/log | logs/s |
|---|---:|---:|
| Log Info / Warn / Error | 0.31 | 3.2 M |
| Log Verbose (literal) | 0.29 | 3.4 M |
| Log with Formatting | 0.40 | 2.5 M |
| Memory literal（NEI） | 0.25–0.26 | 3.9 M |
| File async（NEI） | 1.0–1.3 | 0.84–1.05 M |
| Strict sync flush（NEI） | 107–109 | ~9.4 K |

vs spdlog：memory 场景 NEI 全面更快（0.25–0.36 vs 0.36–0.49 μs）；
file async NEI 快 ~40%（1.0 vs 1.7 μs）；flush-request 场景 spdlog 快
（0.15 vs 0.92 μs，异步 logger + 每次 flush 请求）；strict sync 持平（107 vs 107 μs）。

## File I/O（64 MB，/mnt/c 9p 挂载，最佳块）

| 操作 | 吞吐 | 块 |
|---|---:|---|
| write | **1536.6 MB/s** | 256 KB |
| read | **5517.2 MB/s** | 64 KB |

## Pipe I/O（进程内，64 MB）

| Chunk | Throughput |
|---|---:|
| 4 KB | 3745.5 MB/s |
| 16 KB | 5457.4 MB/s |
| 64 KB | 5415.9 MB/s |
| 256 KB | **5820.8 MB/s** |
| 1 MB | 4911.3 MB/s |

PipeStream 跨进程（64B × 5000）：P50 31.8 μs / P95 49.3 μs，29,388 msgs/s。

## TCP Loopback（1024 MB）

| Buffer | Throughput |
|---|---:|
| 4 KB | 910.7 MB/s |
| 16 KB | 2948.8 MB/s |
| 64 KB | 4404.6 MB/s |
| 128 KB | 6524.4 MB/s |
| 256 KB | 8644.9 MB/s |
| 512 KB | 9700.9 MB/s |
| 1024 KB | **9713.9 MB/s** |

- TCP RTT（1000 连接回环 echo）：avg 15.8 / p99 35.4 ms
- TCP Throughput（10 MB @ 64 KB）：860.6 MB/s
- TCP Conn Stress（1000 连接）：8343.9 conn/s（server accept 计数 546，含 1 例失败——WSL 下 accept 统计受限，吞吐参考性有限）

## TLS Throughput

| Metric | 吞吐 |
|---|---:|
| 10 MB @ 64 KB | **119.6 MB/s** |

## HTTP / HTTP2（回环 keep-alive）

| Bench | 结果 |
|---|---:|
| HTTP/1.1 Throughput（20K req） | 17,637 req/s（P50 56.4 μs） |
| HTTP/2 seq（10K req, concurrency 1） | 13,120 req/s |
| HTTP/2 par8（10K req） | 45,034 req/s |
| HTTP/2 par64（10K req） | **51,480 req/s** |

## 其他单轮 Bench

| Bench | 结果 |
|---|---|
| PostJob 扩展 | 8 线程 666 M ops/s（2.9×）；16 线程回落 439 M（1.9×，WSL 调度器限制） |
| Flake ID | 249.3 M ids/s（8 线程） |
| StringAppendF | vs StringPrintf 1.24×、vs stringstream 1.01×（WSL 下 stringstream 无优势） |
| OnceCallback | 35.3 M/s；RepeatingCallback 103.1 M/s |
| Parallel Runner 原子递增 | 多 worker 0.2×（共享计数器竞争，预期） |
| Task Priority Demo | 三级优先级各 33.3% 公平份额，无饥饿 |

---

## 与 2026-08-18 Windows 同机对比（同 commit）

| Benchmark | Windows | WSL2 | 比值 |
|---|:---:|:---:|:---:|
| task_thread Standard fast-path (off) | 3.96 | 3.74 | 0.94× |
| threadpool Standard (global-heap) | 5.38 | 3.66 | 0.68× |
| threadpool SingleThread dedicated | 4.87 | 3.98 | 0.82× |
| threadpool Parallel single | 3.94 | 4.34 | 1.10× |
| Log Info | 0.275 μs | 0.317 μs | 1.15× |
| async_file best read | 4104.9 | 5517.2 | 1.34× |
| TCP loopback 1024 KB | 3885.7 | 9713.9 | 2.50× |
| TLS throughput | 106.4 | 119.6 | 1.12× |
| HTTP/1.1 Throughput | 22,684 | 17,637 | 0.78× |

> 与历史基线一致：任务调度 Windows 普遍快 10–30%（futex 成本），网络回环 WSL2 更快
> （虚拟网络栈）；Pipe/TCP 优势明显。task 调度关键场景（dedicated 0.82×）为
> 跨平台固有差异，非本次改动引入。
