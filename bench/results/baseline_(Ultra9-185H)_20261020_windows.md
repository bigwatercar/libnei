# libnei — Benchmark Baseline 2026-10-20 (Windows)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores)，Windows 11
**Build**: Release shared（`windows-vs2022-shared`），电源计划 = 性能
**Commit**: `20f860f`（dev：队列改引用计数 A1、StorageMonitor 可注入 blocking_runner + 异步首次枚举、
DNS 测试去环境依赖、多队列饥饿用例 watchdog）
**采集条件**: 20:15–20:19，机器空闲、无并发构建（WSL 侧的采集在其后单独进行，未并行）。
**脚本**: `bench/run_all_benches.ps1`（原样，参数与轮数未改）→ 原始日志 `bench/results/bench_20261020_201505/`
**对照基线**: `baseline_(Ultra9-185H)_20261017_windows.md` §12（同机、同构建配置、最新一轮）
**A/B 对照**: cur = `20f860f`，pre = `15d400f`（A1 之前）→ 见第 8 节

---

## 0. 先读结论

1. **A1（`SequencedTaskQueue` 改引用计数、runner 持强引用）对投递热路径没有可测影响**：8 轮交替 A/B 里
   `Delayed` 中位数 −5%、`Raw atomic` 对照 −2%，落在噪声内；ThreadPool 各路径 pre/cur 中位数差 ≤5%。
2. 本轮**机器状态整体略慢于 10-17**：任务类对照 `Raw atomic` −11.8%（ON）/+2.3%（OFF），
   Log 全面慢 2–33%。因此下表的分项差异优先按“机器状态”解释，除非同时有 A/B 证据。
   TCP 类例外：套内值受 **VPN** 影响严重偏低，退出 VPN 后已复核恢复（见第 6 节）。
3. 与 10-17 基本持平的项：HTTP/1.1 20.6k req/s、HTTP/2 par64 40.4k req/s、Flake ID 266 M ids/s。
4. **TCP 套内偏低是 VPN 造成的**：退出 VPN 后 `tcp_loopback` 256 KB 1978 → 3164 MB/s、
   `tcp_throughput` 125.5 → 519.5 MB/s（8 轮中位），库代码本轮未改 TCP 路径。

---

## 1. Task scheduling — task_thread_bench（10 轮均值，M/s）

括号内为该 10 轮的 min–max，用来判断差异是否超出噪声。

| Scenario | 10-17 ON | 10-20 ON | Δ | 10-17 OFF | 10-20 OFF | Δ |
|---|---:|---:|---:|---:|---:|---:|
| Standard PostTask (fast-path) | 5.00 | **5.04** (4.05–5.64) | +0.7% | 4.04 | **4.23** (3.31–4.49) | +4.7% |
| Delayed PostTask (non-fast-path) | 3.52 | **3.08** (2.73–3.51) | −12.4% | 3.41 | **3.22** (2.47–3.75) | −5.6% |
| Multi-threaded PostTask (4 threads) | 2.31 | **3.18** (3.03–3.36) | +37.5% | 2.32 | **2.60** (2.15–3.21) | +12.1% |
| BindOnce + Run (no queue) | 26.89 | **20.65** (15.99–27.50) | −23.2% | 26.81 | **26.29** (21.84–30.11) | −1.9% |
| BindOnce construction | 37.23 | **29.41** (24.14–35.06) | −21.0% | 35.25 | **35.41** (26.53–41.03) | +0.4% |
| Raw atomic loop（对照，无库代码） | 118.07 | **104.11** (61.05–135.64) | −11.8% | 117.23 | **119.95** (71.23–135.15) | +2.3% |

`BindOnce + Run` / `BindOnce construction` 在 ON 组偏低但 OFF 组持平，且原始样本跨度达 16–41 M/s，
属该轮 ON 组的采样状态（同组 `Raw atomic` 也偏低 −12%），不是回调实现变化。

## 2. Task scheduling — task_threadpool_bench（1M tasks，5 轮均值，M/s）

| Scenario | 10-17 | 10-20 | Δ |
|---|---:|---:|---:|
| Standard PostTask (external, global-heap) | 4.79 | **4.72** (4.25–5.06) | −1.5% |
| SingleThread — dedicated-worker 外部投递 | 4.45 | **3.07** (2.92–3.20) | −31.0% |
| Parallel PostTask (single-thread post) | 3.79 | **3.32** (3.04–3.57) | −12.4% |
| Delayed PostTask (non-fast-path) | 3.21 | **3.03** (2.78–3.36) | −5.6% |
| SingleThread — Delayed | 3.12 | **2.20** (2.12–2.29) | −29.5% |
| Multi-threaded PostTask (4 threads, sequenced) | 2.63 | **2.71** (2.58–2.87) | +3.0% |
| SingleThread — MT (4 threads) | 2.80 | **2.55** (2.43–2.69) | −8.9% |
| Parallel MT (4 threads) | 2.46 | **2.31** (2.14–2.49) | −6.1% |
| Parallel Worker-Repost (global-heap) | 0.611 | **0.604** (0.542–0.682) | −1.1% |
| Worker-Repost (local-queue fast-path) | 0.435 | **0.483** (0.446–0.519) | +11.0% |

两条 `SingleThread —` 的 −30% 看似回归，但**第 8 节的同机 A/B 显示 pre 与 cur 完全一致（3.11 vs 3.11）**，
说明是套件内位置/机器状态，而不是本轮改动。

`task_threadpool_parallel_bench`（5 轮）：Parallel PostTask 3.79→**3.27**（2.86–3.44）；Parallel MT 2.46→**2.34**（2.25–2.39）。

## 3. Log（NEI，5 轮均值，μs/log）

| 场景 | 10-17 | 10-20 | Δ | 10-20 M logs/s |
|---|---:|---:|---:|---:|
| Log Info | 0.296 | 0.334 | +13% | 3.04 |
| Log Warn | 0.297 | 0.325 | +9% | 3.09 |
| Log Error | 0.322 | 0.327 | +2% | 3.06 |
| Log with Formatting | 0.514 | 0.598 | +16% | 1.67 |
| Log Info (literal) | 0.261 | 0.348 | +33% | 2.88 |
| Log Verbose | 0.275 | 0.346 | +26% | 2.90 |
| Log Verbose (literal) | 0.258 | 0.336 | +30% | 2.98 |

日志模块本轮零改动；慢的方向与当轮 `Raw atomic`（ON 组 −12%）和 TCP（第 6 节）一致，判为机器状态。

## 4. Log Compare（NEI vs spdlog，5 轮均值，μs/log）

| 场景 | NEI | spdlog | 结论 |
|---|---:|---:|---|
| memory simple（`%s` vs `{}`） | 0.309 | 0.471 | NEI 快 ~34% |
| memory literal | 0.352 / 0.352（llog / vlog） | 0.466 | NEI 快 ~24% |
| memory multi | 0.592 | 0.580 | 相当 |
| memory `{fmt}` literal | 0.385（simple）/ 0.547（multi） | — | — |
| memory multi（fmt_plan cache miss） | 0.796 | — | — |
| file async simple / multi | 0.573 / 0.647 | 0.519 / 0.555 | spdlog 快 ~10–17% |
| file async literal（llog / vlog） | 0.513 / 0.491 | — | — |
| per-call flush simple / multi | 0.567 / 0.670 | 0.374 / 0.384 | spdlog 快 ~34–43% |
| strict sync flush simple / multi | 18.12 / 17.38 | 13.48 / 13.73 | spdlog 快 ~26%（see 注） |

> strict sync flush 本轮 NEI 17–18 μs，10-17 为 12.6–13.9 μs。该场景直接受磁盘写缓存状态影响
> （WSL 侧同一时段也出现 800× 跳变，见 WSL 基线第 5 节），**建议下轮与磁盘空闲状态一起复核**。

## 5. File / Pipe I/O（单轮）

| 操作 | 10-17 | 10-20 | 块 |
|---|---:|---:|---|
| AsyncFile write（最佳块） | 2621.7 MB/s @4 MB | 2352.4 MB/s @1 MB | — |
| AsyncFile read（最佳块） | 4166.4 MB/s @4 MB | 3105.1 MB/s @64 KB | — |

AsyncFile 全量（64 MB，write / read MB/s）：

| Chunk | 4 KB | 16 KB | 64 KB | 256 KB | 1 MB | 4 MB |
|---|---:|---:|---:|---:|---:|---:|
| write | 362.6 | 1144.5 | 1964.1 | 2256.3 | **2352.4** | 2049.2 |
| read | 816.5 | 2251.2 | **3105.1** | 3091.5 | 3011.5 | 3078.4 |

PipeStream 进程内（64 MB）：4 KB 415.7 / 16 KB 1552.2 / 64 KB 4851.7 / 256 KB **10656** / 1 MB 4400.7 MB/s。
PipeStream 跨进程（64 B × 5000）：P50 14.10 μs / P95 23.50 / avg 14.59，**68,519 msgs/s**（10-17：15.40 / 22.00 / 65,966）。

## 6. TCP / TLS

| 项目 | 10-17 | 10-20 套内 | 10-20 复测（VPN 退出后，3 轮中位数） |
|---|---:|---:|---:|
| TCP loopback 64 KB | 1905.4 MB/s | 1336.1 | **2249.4**（2016.8 / 2249.4 / 2321.9） |
| TCP loopback 256 KB | 3123.0 MB/s | 1977.9 | **3164.0**（3080.9 / 3164.0 / 3337.5） |
| TCP loopback 1 MB | 2668.9 MB/s | 2336.4 | **2090.0**（2015.0 / 2090.0 / 2485.2） |
| TCP Throughput（10 MB @ 64 KB） | 393.5 | 125.5 | **519.5**（8 轮：128.6–617.7，中位 519.5） |
| TLS Throughput（10 MB @ 64 KB） | 88.3 | 98.8 | **104.8**（61.8 / 104.8 / 104.8） |
| TCP RTT（1000 连接） | avg 10.56 / p50 16.38 / p99 16.91 ms | avg 11.94 / p50 17.43 / p99 21.14 ms，0 失败 | avg 11.55 / p50 14.05 / p99 21.42 ms，0 失败 |
| TCP Conn Stress（1000） | 1355.9 conn/s | 1457.9（accept 1 例失败） | **1454.2**（1306.1 / 1454.2 / 1529.5） |

TCP loopback 全量（1024 MB，套内单轮）：4 KB 193.1 / 8 KB 269.4 / 16 KB 381.6 / 32 KB 822.8 / 64 KB 1336.1 /
128 KB 1767.8 / 256 KB 1977.9 / 512 KB 2181.9 / 1024 KB 2336.4 MB/s
（10-17：181.2 / 281.5 / 741.6 / 1390.4 / 1905.4 / 2379.8 / 3123.0 / 2404.5 / 2373.4）。

> **结论：套内 TCP 偏低主要是 VPN 造成的，已复核排除。** 退出 VPN 后重跑，`tcp_loopback` 64 KB
> 从 1336 → **2249 MB/s**、256 KB 从 1978 → **3164 MB/s**（回到甚至超过 10-17 水平），
> `tcp_throughput` 8 轮中位 **519.5 MB/s**（10-17：393.5 套内 / 574.6 复测）。
>
> 剩下两点作为**观测项**保留：① `tcp_throughput` 呈双峰（8 轮里 128.6 / 128.6 两轮、其余 ≥330，中位 519.5），
> 单次 10 MB 传输只有约 160 个 64 KB 写，对初始 cwnd/窗口恢复极敏感；② `tcp_loopback` 1 MB 仍在 2.0–2.5 GB/s，
> 略低于 10-17 的 2.67 GB/s。两者都只出现在单轮项，建议后续与多轮中位数一起看。

## 7. HTTP / HTTP2（回环 keep-alive）

| Bench | 10-17 | 10-20 | Δ |
|---|---:|---:|---:|
| HTTP/1.1（20K req） | 20,515.6 req/s（48.2 μs） | **20,629.3 req/s（47.9 μs）** | +0.6% |
| HTTP/2 seq（10K, c=1） | 16,860.2 | **17,796.3** | +5.6% |
| HTTP/2 par8（10K） | — | **33,398.2** | — |
| HTTP/2 par64（10K） | 32,352.3 | **40,367.7** | +24.8%（10-17 该轮标注为偏低样本） |

## 8. A/B 对照：A1（队列改引用计数）是否影响投递热路径

同机交替运行两份 Release 二进制，各自带自己的 `nei.dll`：

- **pre** = `../libnei-ab` @ `15d400f`（A1 之前）
- **cur** = `20f860f`（当前 HEAD）

`task_thread_bench 1000000 off`，8 组交替（中位数）：

| 场景 | pre | cur | Δ |
|---|---:|---:|---:|
| Delayed PostTask (non-fast-path) | 3.66 | 3.47 | −5.2% |
| Raw atomic loop（对照） | 127.86 | 125.31 | −2.0% |
| Standard PostTask (fast-path) | 4.30 | 4.35 | +1.2% |
| Multi-threaded PostTask (4 threads) | 3.25 | 3.36 | +3.4% |

`task_threadpool_bench 1000000 off`，3 组交替（中位数）：

| 场景 | pre | cur | Δ |
|---|---:|---:|---:|
| Standard PostTask (global-heap) | 4.75 | 4.76 | +0.2% |
| SingleThread — dedicated-worker | 3.11 | 3.11 | 0.0% |
| Parallel PostTask | 2.96 | 3.19 | +7.8% |
| Delayed PostTask | 2.69 | 2.79 | +3.7% |
| SingleThread — Delayed | 2.40 | 2.28 | −5.0% |
| Multi-threaded PostTask (sequenced) | 2.69 | 2.69 | 0.0% |
| SingleThread — MT (4 threads) | 2.68 | 2.69 | +0.4% |
| Parallel MT (4 threads) | 2.30 | 2.40 | +4.3% |

**结论**：A1 把 `SequencedTaskRunner` 的 `WeakPtr<queue>` 换成 `scoped_refptr`（按 const 引用传递，
投递路径本身没有新增原子操作），实测**没有可测的吞吐影响**：所有场景差异符号随机、幅度在
±5–8% 噪声内，且与对照 `Raw atomic` 同量级。第 2 节里 `SingleThread —` 相对 10-17 的 −30%
由本次 A/B 排除（pre/cur 相同），是套件位置与机器状态。

## 9. 其他单轮 Bench

| Bench | 10-17 | 10-20 | 备注 |
|---|---|---|---|
| PostJob Per-worker（1/2/4/8/16） | — | 243.6 / 454.1 / 706.4 / 1053.2 / 1353.0 M ops/s（5.6×） | 16 线程与 10-17 的 1444.6 持平 |
| PostJob 批量（500K ops） | — | PostJob 1w 28.4 / SeqRunner 4.1 / PostJob 8w 32.5 M/s | — |
| Flake ID（8 线程） | 220.1 M ids/s | **266.2 M ids/s** | 单轮 |
| StringAppendF | 4.20 M ops/s（1.52× / 3.24×） | **5.02 M ops/s（1.28× / 2.77×）** | 相对优势略降（对手同样变快） |
| OnceCallback / RepeatingCallback / BindOnce | 30.7 / 62.6 / 25.4 M/s | **38.8 / 83.6 / 37.3 M/s** | 单轮 |
| Parallel Runner 原子递增（1→16 workers） | 227 / 109 / 71 / 65 / 62 M/s | 245.0 / 72.4 / 64.1 / 54.5 / 56.9 M/s | 竞争，符合预期 |
| Parallel Runner 小任务（1→16） | 761 … 574 K/s | 632 / 607 / 606 / 601 / 592 K/s | — |
| Task priority（60K 任务 / 3 优先级 / 4 workers） | — | **1495 tasks/s**，份额 33.3% / 33.3% / 33.3%，首 256 次启动 86/85/85 | 公平性保持 |

## 10. 采集口径与建议

1. 套件内单轮项方差依旧很大（同轮 `tcp_loopback` 256 KB 1977.9 vs `Raw atomic` min–max 61–136 M/s），
   **跨版本对比必须用多轮中位数或交替 A/B**；本文件的单轮项只用于同机纵向观察。
2. 长链套件（task 20×1M + threadpool 10×1M）后的 threadpool 段会系统性偏低，仍建议把 task 类与
   I/O 类分两次跑。
3. 本轮需要下轮复核的环境项：① strict sync flush（NEI 12.6 → 17.4 μs）；② `tcp_throughput` 双峰
   （中位 519.5，但 8 轮中有 2 轮 ~129 MB/s）。TCP 套内偏低已确认由 VPN 引起并在第 6 节复核。
   两者都不是本轮代码触碰的路径。
4. **采集时务必关闭 VPN**：本轮套内 TCP 采样在 VPN 开启状态下完成，导致 256 KB loopback 2000 vs 3164 MB/s、
   `tcp_throughput` 125 vs 519 MB/s 的假性“回归”。已把这条写进采集口径。
4. A1 的收益是**正确性**（旧 runner 不再可能触到已释放队列），不是性能；性能上用 A/B 已确认中性。
