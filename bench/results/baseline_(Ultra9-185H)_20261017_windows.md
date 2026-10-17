# libnei — Benchmark Baseline 2026-10-17 (Windows)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores), Windows 11
**Build**: Release shared（`windows-vs2022-shared`）
**Commit**: `91af2a7`（dev：线程池修复 + 全库 ASAN/LSan 泄漏清零 + StorageMonitor 已在分支内）
**采集条件**: 21:28–21:40，机器空闲（采样 CPU 6–9%），无后台高负载。
**脚本**: `bench/run_all_benches.ps1`（原样，参数与轮数未改）→ 原始日志 `bench/results/bench_20261017_212830/`
**对照基线**: `baseline_(Ultra9-185H)_20261014_windows.md`（同机、同构建配置、v0.9.1 `6031c9b`）

> **先读第 8 节**：本轮做了 pre/post A/B 对照，结论是**本轮改动没有可测的吞吐影响**；
> 下文与 10-14 的差异主要来自机器状态（单轮 bench 方差可达 2–4×）。

---

## 1. Task scheduling — task_thread_bench（10 轮均值，M/s）

| Scenario | 10-14 ON | 10-17 ON | Δ | 10-14 OFF | 10-17 OFF | Δ |
|---|---:|---:|---:|---:|---:|---:|
| Standard PostTask (fast-path) | 5.37 | 5.45 | +1.5% | 4.30 | 4.16 | −3.3% |
| Delayed PostTask (non-fast-path) | 3.98 | 3.76 | −5.5% | 3.65 | 3.64 | −0.3% |
| Multi-threaded PostTask (4 threads) | 2.54 | 2.44 | −3.9% | 2.68 | 2.48 | −7.5% |
| BindOnce + Run (no queue) | 29.83 | 28.52 | −4.4% | 29.52 | 26.90 | −8.9% |
| BindOnce construction | 40.96 | 38.97 | −4.9% | 39.86 | 38.61 | −3.1% |
| Raw atomic loop（对照，无库代码） | 131.7 | 127.5 | −3.2% | 132.5 | 130.6 | −1.4% |

Raw atomic 是纯原子循环对照，−3% 即本轮机器状态与上次的差距量级；其余项差异均在同一量级内。

## 2. Task scheduling — task_threadpool_bench（1M tasks，M/s）

| Scenario | 10-14 | 10-17（套内 5 轮均值） | 10-17（单独复测 6 轮中位数） | 备注 |
|---|---:|---:|---:|---|
| Standard PostTask (external, global-heap) | 5.25 | 4.39 | 4.88 | A/B cur/pre = +3.6% |
| SingleThread — dedicated-worker 外部投递 | 5.05 | 3.25 | 4.55 | A/B −0.1% |
| Parallel PostTask (single-thread post) | 4.32 | 3.03 | 3.75 | A/B +1.5% |
| SingleThread — Delayed | 4.00 | 2.18 | 3.75 | A/B +5.5% |
| Delayed PostTask (non-fast-path) | 3.48 | 2.83 | 3.31 | A/B −7.3% |
| Multi-threaded PostTask (4 threads, sequenced) | 3.16 | 2.43 | 2.62 | A/B −1.5% |
| SingleThread — MT (4 threads) | 3.04 | 2.33 | 2.73 | A/B +0.4% |
| Parallel MT (4 threads) | 2.61 | 2.40 | 2.41 | A/B −3.2% |
| Parallel Worker-Repost (global-heap) | 0.75 | 0.56 | 0.63 | A/B +5.0% |
| Worker-Repost (local-queue fast-path) | 0.56 | 0.44 | 0.47 | A/B +9.8% |

`task_threadpool_parallel_bench`（5 轮）：Parallel PostTask 3.85→3.19（套内）/ 3.75（复测）；Parallel MT 2.66→2.31 / 2.41。

**套内数值系统性低于单独复测**：套件里这 5 轮紧跟在 task_thread 的 20×1M 之后，CPU 处于长时高负载后的降频/调度状态；
单独复测（交替 A/B）才反映真实性能。

## 3. Log（NEI，5 轮均值）

| 场景 | 10-14 μs/log | 10-17 μs/log | Δ | 10-17 logs/s |
|---|---:|---:|---:|---:|
| Log Info | 0.39 | 0.302 | −23% | 3.32 M |
| Log Warn | 0.39 | 0.297 | −24% | 3.39 M |
| Log Error | 0.40 | 0.312 | −22% | 3.24 M |
| Log with Formatting | 0.73 | 0.555 | −24% | 1.81 M |
| Log Info (literal) | 0.33 | 0.286 | −13% | 3.50 M |
| Log Verbose | 0.39 | 0.297 | −24% | 3.38 M |
| Log Verbose (literal) | 0.35 | 0.274 | −22% | 3.65 M |

Log 路径本轮反而更快（内存 sink 场景 +13%~+30% logs/s）；与日志模块相关的代码本轮零改动，同样归因于机器状态（与 pipe/TCP 的下降方向相反，说明不是单一整体降频）。

## 4. Log Compare（NEI vs spdlog，5 轮均值，μs/log）

| 场景 | NEI | spdlog | 结论 |
|---|---:|---:|---|
| memory simple（`%s` vs `{}`） | 0.308 | 0.501 | NEI 快 ~39% |
| memory literal | 0.311 / 0.280（llog / vlog） | 0.217 | spdlog 快 ~31–43% |
| memory multi | 0.539 | 0.569 | 相当 |
| memory `{fmt}` literal | 0.361（simple）/ 0.477（multi） | — | — |
| memory multi（fmt_plan cache miss） | 0.571 | — | — |
| file async simple | 0.408 | 0.379 | spdlog 快 ~8% |
| file async multi | 0.502 | 0.473 | spdlog 快 ~6% |
| per-call flush simple | 0.458 | 0.254 | spdlog 快 ~45% |
| per-call flush multi | 0.343 | 0.353 | 相当 |
| strict sync flush simple | 13.92 | 15.34 | NEI 快 ~9% |
| strict sync flush multi | 12.57 | 2.80 | spdlog 单轮异常值，不可比 |

## 5. File / Pipe I/O

| 操作 | 10-14 | 10-17（套内） | 10-17（复测） | 块 |
|---|---:|---:|---:|---|
| write | 2981.3 MB/s | 2740.5 MB/s | 2879.6 / 2778.0 MB/s | 4 MB / 1 MB |
| read | 4025.9 MB/s | 3902.0 MB/s | 3687.3 / 3494.2 MB/s | 4 MB / 256 KB |

PipeStream 进程内（64 MB）：

| Chunk | 10-14 | 10-17（套内） | 10-17（3 次复测） |
|---|---:|---:|---|
| 4 KB | 411.9 | 399.5 | — |
| 16 KB | 1551.4 | 595.5 | 1466.8 / 600.2 / 1456.4 |
| 64 KB | 4825.4 | 2060.9 | 4662.3 / 2131.0 / 4844.8 |
| 256 KB | **11074** | 6183.5 | 2145.4 / 5527.7 / 9954.8 |
| 1 MB | 5104.8 | 3356.4 | — |

PipeStream 跨进程（64 B × 5000）：P50 15.8 μs / P95 27.0 μs / avg 16.9 μs，59,183 msgs/s（10-14：14.6 / 18.4 / 65,935）。

> 16 KB 三次复测 1467 / 600 / 1456 MB/s —— 同一进程状态下 2.4× 抖动，说明这类 bench 单轮不可比。

## 6. TCP / TLS

| 项目 | 10-14 | 10-17（套内） | 10-17（复测） |
|---|---:|---:|---|
| TCP loopback 256 KB | 3595.3 MB/s | 3123.0 | 1260.0 / 4033.5 |
| TCP loopback 1 MB | 2363.9 MB/s | 2373.4 | 3406.7 / 2919.5 |
| TCP RTT（1000 连接） | avg 10.7 / p50 15.8 / p99 18.9 ms | avg 11.2 / p50 16.3 / p99 20.5 ms | 0 失败 |
| TCP Throughput（10 MB @ 64 KB） | 548.0 MB/s | 456.4 | 574.6 / 306.3 / 486.1 |
| TCP Conn Stress（1000） | 1403.5 conn/s | 1344.5（accept 1 例失败） | — |
| TLS Throughput（10 MB @ 64 KB） | 100.9 MB/s | 82.8 | 102.7 / 103.1 / 104.3 |

TCP loopback 全量（套内，1024 MB）：4 KB 181.2 / 8 KB 281.5 / 16 KB 741.6 / 32 KB 1390.4 / 64 KB 1905.4 / 128 KB 2379.8 / 256 KB 3123.0 / 512 KB 2404.5 / 1024 KB 2373.4 MB/s。

## 7. HTTP / HTTP2（回环 keep-alive）

| Bench | 10-14 | 10-17（套内） | 10-17（复测 / A/B） |
|---|---:|---:|---|
| HTTP/1.1（20K req） | 24,449 req/s（40.4 μs） | 18,589（53.2 μs） | 21,111 / 21,359 / 20,653；A/B cur 20.6–21.1k |
| HTTP/2 seq（10K, c=1） | 20,899 | 17,812 | A/B cur 17.3–17.9k |
| HTTP/2 par8（10K） | 42,764 | 38,573 | — |
| HTTP/2 par64（10K） | 49,582 | 44,429 | 44,861 / 40,932；A/B cur 44.5–47.2k |

## 8. A/B 对照：本轮改动是否有回归

同一时段交替运行（pre = `afa65d0`，本轮三个线程池修复之前的提交；cur = `91af2a7`；两份二进制各带自己的 `nei.dll`）：

| 场景 | pre 中位数 | cur 中位数 | Δ |
|---|---:|---:|---:|
| Standard（global-heap） | 4.71 | 4.88 | +3.6% |
| SingleThread dedicated | 4.56 | 4.55 | −0.1% |
| Parallel PostTask | 3.70 | 3.75 | +1.5% |
| Worker-Repost（local） | 0.43 | 0.47 | +9.8% |
| Parallel Worker-Repost | 0.60 | 0.63 | +5.0% |
| Delayed | 3.57 | 3.31 | −7.3% |
| SingleThread Delayed | 3.56 | 3.75 | +5.5% |
| MT 4 (sequenced) | 2.66 | 2.62 | −1.5% |
| Parallel MT | 2.49 | 2.41 | −3.2% |
| HTTP/1.1 req/s | 20,809 | 21,084 | +1.3% |
| HTTP/2 seq req/s | 17,999 | 17,823 | −1.0% |
| HTTP/2 par64 req/s | 46,304 | 46,691 | +0.8% |
| PostJob 16 workers | 1798 / 1552 M/s | 1643 / 1543 M/s | 噪声内 |

**结论**：本轮（线程池语义修复 + ASAN 泄漏清零 + StorageMonitor）在吞吐上没有可测影响，
差异符号随轮次翻转、幅度落在 ±10% 噪声内；per-task 的 `WillRunTask()` 记账开销远小于噪声。

## 9. 其他单轮 Bench

| Bench | 10-14 | 10-17 | 备注 |
|---|---|---|---|
| PostJob 扩展（1/2/4/8/16 线程） | 16 线程 1441 M/s（5.8×） | 214 / 363 / 584 / 973 / 915 M/s | A/B 复测 16 线程 1543–1798 M/s，套内值为低抽样 |
| Flake ID（8 线程） | 280.0 M ids/s | 200.4 M ids/s | 单轮 |
| StringAppendF | 5.02 M ops/s（1.18× / 2.74×） | 4.20 M ops/s（1.52× / 3.24×） | 相对优势反而扩大 |
| OnceCallback / RepeatingCallback / BindOnce | 39.3 / 85.1 / 38.3 M/s | 30.7 / 62.6 / 25.4 M/s | 单轮，同机器状态偏低 |
| Parallel Runner 原子递增（1→16 workers） | 0.3×（竞争，符合预期） | 227 / 109 / 71 / 65 / 62 M/s（0.5→0.3×） | 预期行为 |
| Parallel Runner 小任务（1→16 workers） | — | 761 / 641 / 627 / 605 / 574 K/s | — |

---

## 10. 采集口径与建议

1. 套件内单次运行（`run_all_benches.ps1` 的单轮 bench）方差极大：同一构建同一时段，
   `pipe_stream` 16 KB 出现 1467 / 600 / 1456 MB/s，`tcp_loopback` 256 KB 出现 1260 / 4033 MB/s，
   WSL 侧 `post_job` 16 线程出现 420 / 954 / 1663 M/s。**跨版本对比必须用多轮中位数或交替 A/B**。
2. 长链 bench（task_thread 20×1M）之后的 threadpool 段会系统性偏低 15–30%，建议把 task 类基准
   与 I/O 类基准分两次运行。
3. 本轮相对 10-14 的普降（多集中在 I/O 与 http/tcp 类）与机器状态一致：Log 路径反而变快、
   Raw atomic 只差 3%，说明不是整体降频，而是分支预测/亲和性/后台活动层面的波动。
4. 与 10-14 相比唯一稳定偏低且值得下轮复看的是 HTTP/1.1（24.4k → 21k req/s，−14%），
   A/B 已排除本轮改动，若关心需与 v0.9.1 二进制在同一时段交替复测。
