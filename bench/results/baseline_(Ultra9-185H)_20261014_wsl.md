# libnei — Benchmark Baseline 2026-10-14 (WSL2)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores), WSL2 Ubuntu-24.04（内核 6.18.33.2-microsoft-standard-WSL2），GCC Release shared
**Build**: Release shared（`~/libnei_build/wsl-release-shared`，源码取自 `/mnt/c/.../libnei-src`）
**Commit**: `6031c9b` — build(version): bump to 0.9.1 for release（v0.9.1）
**采集条件**: 机器空闲（WSL 启动时 loadavg 0.2），与同日 Windows 基线 `baseline_(Ultra9-185H)_20261014_windows.md` 同机同提交。

> **口径变化（与 08-18 WSL 基线不可直接对比的项）**：本轮 log / async_file 的临时文件位于 WSL 原生 ext4（`/tmp`），
> 08-18 位于 `/mnt/c` 9p 挂载。因此 File I/O、Log 的 file 场景、strict sync flush 数值不具可比性（见各节说明）。

---

## Task Scheduling（均值，1M tasks，单位 M/s）

### task_thread_bench（10 轮）

| Scenario | tracing ON | tracing OFF |
|---|---:|---:|
| Standard PostTask (fast-path) | 2.51 | 2.62 |
| Delayed PostTask (non-fast-path) | 3.35 | 3.71 |
| Multi-threaded PostTask (4 threads) | 2.87 | 3.04 |
| BindOnce + Run (no queue) | 28.29 | 28.28 |
| BindOnce construction | 36.91 | 36.21 |
| Raw atomic loop（对照） | 133.2 | 129.6 |

### task_threadpool_bench（5 轮，tracing off）

| Scenario | M/s |
|---|---:|
| Parallel PostTask (single-thread post) | 3.96 |
| SingleThread — dedicated-worker 外部投递 | 2.83 |
| SingleThread — Delayed | 2.68 |
| SingleThread — MT (4 threads) | 2.37 |
| Standard PostTask (external, global-heap) | 2.33 |
| Multi-threaded PostTask (4 threads, sequenced) | 2.20 |
| Delayed PostTask (non-fast-path) | 1.93 |
| Parallel MT (4 threads) | 1.79 |
| Parallel Worker-Repost (global-heap) | 0.36 |
| Worker-Repost (local-queue fast-path) | 0.25 |

### task_threadpool_parallel_bench（5 轮）

| Scenario | M/s |
|---|---:|
| Parallel PostTask (single-thread post) | 3.69 |
| Parallel MT (4 threads) | 1.60 |

## Log（5 轮均值，日志落 `/tmp` ext4）

| 场景 | E2E μs/log | logs/s |
|---|---:|---:|
| Log Info / Warn / Error | 0.30–0.31 | 3.20–3.30 M |
| Log Verbose (literal) | 0.29 | 3.51 M |
| Log Info (literal) | 0.29 | 3.43 M |
| Log with Formatting | 0.37 | 2.69 M |
| Memory literal（NEI llog / vlog_literal） | 0.25–0.26 | 3.9 M |
| Memory simple %s（NEI） | 0.27 | 3.74 M |
| File async（NEI） | 0.27–0.39 | 2.6–3.7 M |
| Strict sync flush（NEI） | 0.70 | ~1.44 M |

vs spdlog：memory literal 0.413 vs NEI 0.257（NEI 快 ~38%）；simple 0.448 vs 0.267（NEI 快 ~40%）；multi 0.632 vs 0.346（NEI 快 ~45%）；
file async 0.33–0.35 vs 0.27–0.31（NEI 快 ~10–20%）；flush-request 场景 NEI 0.29–0.34 vs spdlog 0.42–0.45（NEI 快 ~30%）；
strict sync flush NEI 0.70 vs spdlog 0.47–0.50（spdlog 快 ~30%）。

> ⚠️ strict sync flush 在 `/tmp` ext4 上仅 0.7 μs/log（08-18 在 9p 上为 ~107 μs），显然未包含真实落盘代价（WSL 虚拟磁盘写缓存），
> 仅作 NEI/spdlog 相对比较参考，不要与 Windows（11 μs）或 08-18 对比。

## File I/O（64 MB，`/tmp` ext4，最佳块）

| 操作 | 吞吐 | 块 |
|---|---:|---|
| write | **1939.2 MB/s** | 4 MB |
| read | **6074.4 MB/s** | 64 KB |

（本轮 bench 块大小集合为 64 KB / 256 KB / 1 MB / 4 MB；读为页缓存命中。08-18 在 `/mnt/c` 9p 上：write 1536.6 / read 5517.2。）

## Pipe I/O（进程内，64 MB）

| Chunk | Throughput |
|---|---:|
| 4 KB | 4581.9 MB/s |
| 16 KB | 6606.1 MB/s |
| 64 KB | 6816.4 MB/s |
| 256 KB | 7067.9 MB/s |
| 1 MB | **7422.0 MB/s** |

PipeStream 跨进程（64B × 5000）：P50 30.8 μs / P95 44.0 μs，32,195 msgs/s。

## TCP Loopback（1024 MB）

| Buffer | Throughput |
|---|---:|
| 4 KB | 962.0 MB/s |
| 8 KB | 1876.7 MB/s |
| 16 KB | 2913.1 MB/s |
| 32 KB | 4330.5 MB/s |
| 64 KB | 5056.5 MB/s |
| 128 KB | 6139.0 MB/s |
| 256 KB | 7802.7 MB/s |
| 512 KB | 9866.5 MB/s |
| 1024 KB | **10142.7 MB/s** |

- TCP RTT（1000 连接回环 echo）：avg 23.8 / p50 38.8 / p99 92.3 ms（0 失败）
- TCP Throughput（10 MB @ 64 KB）：822.8 MB/s
- TCP Conn Stress（1000 连接）：15,632.7 conn/s（server accept 计数 585，含 1 例失败——WSL 下 accept 统计受限，吞吐参考性有限）

## TLS Throughput

| Metric | 吞吐 |
|---|---:|
| 10 MB @ 64 KB | **132.6 MB/s** |

## HTTP / HTTP2（回环 keep-alive）

| Bench | 结果 |
|---|---:|
| HTTP/1.1 Throughput（20K req） | 17,963 req/s（avg 55.4 μs） |
| HTTP/2 seq（10K req, concurrency 1） | 13,533 req/s |
| HTTP/2 par8（10K req） | 40,785 req/s |
| HTTP/2 par64（10K req） | **52,325 req/s** |

## 其他单轮 Bench

| Bench | 结果 |
|---|---|
| PostJob 扩展 | 8 线程 734 M ops/s（3.0×）；16 线程 1546 M（6.3×） |
| Flake ID | 255.3 M ids/s（8 线程） |
| StringAppendF | 10.93 M ops/s（vs StringPrintf 1.22×、vs stringstream 1.07×） |
| OnceCallback | 39.0 M/s；RepeatingCallback 103.6 M/s；BindOnce 25.4 M/s |
| Parallel Runner 原子递增 | 16 workers 0.3×（共享计数器竞争，预期） |
| Task Priority Demo | 三级优先级各 33.3% 公平份额，无饥饿（1498 tasks/s） |

---

## 与 2026-08-18 WSL 基线对比

| 项目 | 08-18 | 10-14 | 变化 |
|---|---:|---:|---:|
| task_thread Standard (fast-path, ON) | 3.60 | 2.51 | **−30%** |
| task_thread Standard (fast-path, OFF) | 3.74 | 2.62 | **−30%** |
| task_thread Delayed (ON) | 3.79 | 3.35 | −12% |
| task_thread MT 4t (ON) | 2.99 | 2.87 | −4% |
| task_thread BindOnce + Run | 28.82 | 28.29 | −2% |
| task_thread BindOnce construction | 38.21 | 36.91 | −3% |
| task_threadpool Standard (global-heap) | 3.66 | 2.33 | **−36%** |
| task_threadpool SingleThread dedicated | 3.98 | 2.83 | **−29%** |
| task_threadpool Delayed | 2.27 | 1.93 | −15% |
| task_threadpool Parallel PostTask | 4.34 | 3.96 | −9% |
| task_threadpool Parallel MT | 1.91 | 1.79 | −6% |
| task_threadpool Worker-Repost (local) | 0.29 | 0.25 | −12% |
| Pipe 256 KB (MB/s) | 5820.8 | 7067.9 | +21% |
| TCP loopback 1024 KB (MB/s) | 9713.9 | 10142.7 | +4% |
| TLS 10 MB (MB/s) | 119.6 | 132.6 | +11% |
| HTTP/1.1 (20K, req/s) | 17,637 | 17,963 | +2% |
| HTTP/2 par64 (req/s) | 51,480 | 52,325 | +2% |

## 结论

- 网络 / Pipe / HTTP / TLS / BindOnce 与 08-18 持平或更优，无回归迹象。
- **WSL 上 Standard PostTask 路径较 08-18 回落约 30%**（task_thread fast-path 3.7→2.6；threadpool global-heap 3.66→2.33，dedicated 3.98→2.83）。
  复跑 3 次 task_threadpool_bench，global-heap 为 1.95–2.32、dedicated 2.54–2.86，结果稳定复现，不是单轮噪声。
  同提交的 Windows 同类场景反而回升（task_thread Standard 4.44→5.37），故问题仅在 WSL/Linux 路径。
  **未确认根因**：08-18（`1136ec4`）到 `6031c9b` 之间的代码变更与 WSL 内核/环境变化均可能；需在同一环境下构建 `1136ec4` 对照才能区分。
- File / Log file / strict flush 因临时目录由 9p 改为 ext4，不可与 08-18 对比。
