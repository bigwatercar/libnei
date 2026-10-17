# libnei — Benchmark Baseline 2026-10-17 (WSL2)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores)，WSL2 Ubuntu-24.04（内核 6.18.33.2-microsoft-standard-WSL2），GCC Release shared
**Build**: `~/libnei_build/wsl-release-shared`（源码 `/mnt/c/.../libnei-src`）
**Commit**: `91af2a7`（dev，与同日 Windows 基线 `baseline_(Ultra9-185H)_20261017_windows.md` 同提交）
**采集条件**: 21:32–21:40，启动时 loadavg 0.18，结束后 3.56（源自基准本身）。
**运行方式**: `bench/run_all_benches.ps1` 依赖 Windows 命名（`.exe`、反斜杠 `Join-Path`），WSL 无法直接执行；
本轮用**参数与轮数完全相同**的 bash 驱动跑同一批二进制（原始日志 `/tmp/wsl_bench_20261017_213227/`）。
**对照基线**: `baseline_(Ultra9-185H)_20261014_wsl.md`（同机同配置）
**最新一轮**: 见文末第 10 节（22:35 采集，`steady_clock` 修复后，以该节数值为准）

---

## 0. 先说结论

WSL 侧本轮**整体比 10-14 慢约 10–13%**，对照组 `Raw atomic loop`（纯原子循环，不含库代码）
由 133.2/129.6 降到 119.0/112.5（−10.7%/−13.2%），因此绝大多数分项差异属于**机器状态差异**，
不是代码回归。Windows 侧同日做了 pre/post A/B，已证明本轮改动无吞吐影响（见 Windows 基线第 8 节）。

---

## 1. Task scheduling — task_thread_bench（10 轮均值，M/s）

| Scenario | 10-14 ON | 10-17 ON | Δ | 10-14 OFF | 10-17 OFF | Δ |
|---|---:|---:|---:|---:|---:|---:|
| Standard PostTask (fast-path) | 2.51 | 2.46 | −2.0% | 2.62 | 2.34 | −10.7% |
| Delayed PostTask (non-fast-path) | 3.35 | 2.96 | −11.6% | 3.71 | 3.18 | −14.3% |
| Multi-threaded PostTask (4 threads) | 2.87 | 2.50 | −12.9% | 3.04 | 2.59 | −14.8% |
| BindOnce + Run (no queue) | 28.29 | 25.76 | −8.9% | 28.28 | 24.59 | −13.0% |
| BindOnce construction | 36.91 | 33.50 | −9.2% | 36.21 | 31.55 | −12.9% |
| Raw atomic loop（对照） | 133.2 | 118.97 | −10.7% | 129.6 | 112.49 | −13.2% |

## 2. Task scheduling — task_threadpool_bench（1M tasks，5 轮均值，M/s）

| Scenario | 10-14 | 10-17 | Δ |
|---|---:|---:|---:|
| Parallel PostTask (single-thread post) | 3.96 | 3.09 | −22.0% |
| SingleThread — dedicated-worker 外部投递 | 2.83 | 2.45 | −13.4% |
| SingleThread — Delayed | 2.68 | 2.19 | −18.3% |
| SingleThread — MT (4 threads) | 2.37 | 1.94 | −18.1% |
| Standard PostTask (external, global-heap) | 2.33 | 2.06 | −11.6% |
| Multi-threaded PostTask (4 threads, sequenced) | 2.20 | 1.89 | −14.1% |
| Delayed PostTask (non-fast-path) | 1.93 | 1.77 | −8.3% |
| Parallel MT (4 threads) | 1.79 | 1.54 | −14.0% |
| Parallel Worker-Repost (global-heap) | 0.36 | 0.305 | −15.3% |
| Worker-Repost (local-queue fast-path) | 0.25 | 0.225 | −10.0% |

`task_threadpool_parallel_bench`（5 轮）：Parallel PostTask 3.69→3.05（−17.3%）；Parallel MT 1.60→1.46（−8.8%）。

> 除 Parallel PostTask（−22%）外的所有项都落在对照组的 −10~−15% 区间内；Parallel PostTask 的
> Windows A/B 显示本轮改动为 +1.5%，故判定为状态噪声。

## 3. Log（NEI，5 轮均值，日志落 `/tmp` ext4）

| 场景 | 10-14 μs/log | 10-17 μs/log | Δ |
|---|---:|---:|---:|
| Log Info | 0.30 | 0.362 | +21% |
| Log Warn | 0.31 | 0.345 | +11% |
| Log Error | 0.31 | 0.354 | +14% |
| Log with Formatting | 0.37 | 0.439 | +19% |
| Log Info (literal) | 0.29 | 0.336 | +16% |
| Log Verbose | 0.30 | 0.354 | +18% |
| Log Verbose (literal) | 0.29 | 0.344 | +19% |

## 4. Log Compare（NEI vs spdlog，5 轮均值，μs/log）

| 场景 | NEI | spdlog | 结论 |
|---|---:|---:|---|
| memory simple（`%s` vs `{}`） | 0.321 | 0.461 | NEI 快 ~30% |
| memory literal | 0.300 / 0.280（llog / vlog） | 0.420 | NEI 快 ~28–33% |
| memory multi | 0.412 | 0.684 | NEI 快 ~40% |
| memory `{fmt}` literal | 0.327（simple）/ 0.437（multi） | — | — |
| memory multi（fmt_plan cache miss） | 0.437 | — | — |
| file async simple | 0.313 | 0.322 | 相当 |
| file async multi | 0.342 | 0.410 | NEI 快 ~17% |
| per-call flush simple | 0.335 | 0.425 | NEI 快 ~21% |
| per-call flush multi | 0.342 | 0.414 | NEI 快 ~17% |
| strict sync flush simple | 0.749 | 0.547 | spdlog 快 ~27% |
| strict sync flush multi | 0.810 | 0.581 | spdlog 快 ~28% |

> strict sync flush 在 `/tmp` ext4 上仍是亚微秒级（未含真实落盘代价，WSL 虚拟盘写缓存），
> 只能做 NEI/spdlog 相对比较，不可与 Windows（12–16 μs）对比。

## 5. File / Pipe I/O

| 项目 | 10-14 | 10-17 | Δ |
|---|---:|---:|---:|
| write（最佳块） | 1939.2 MB/s @ 4 MB | 2072.9 MB/s @ 1 MB | +6.9% |
| read（最佳块） | 6074.4 MB/s @ 64 KB | 4861.4 MB/s @ 64 KB | −20.0% |

PipeStream 进程内（64 MB）：

| Chunk | 10-14 | 10-17 | Δ |
|---|---:|---:|---:|
| 4 KB | 4581.9 | 4266.9 | −6.9% |
| 16 KB | 6606.1 | 6501.4 | −1.6% |
| 64 KB | 6816.4 | 6953.4 | +2.0% |
| 256 KB | 7067.9 | 7032.9 | −0.5% |
| 1 MB | **7422.0** | 6815.0 | −8.2% |

PipeStream 跨进程（64 B × 5000）：P50 33.75 μs（10-14 30.8）/ P95 50.56（44.0）/ max 1027.93，28,366 msgs/s（32,195，−12%）。

## 6. TCP / TLS

| 项目 | 10-14 | 10-17 | Δ |
|---|---:|---:|---:|
| TCP loopback 4 KB | 962.0 | 927.9 | −3.5% |
| TCP loopback 64 KB | 5056.5 | 4393.9 | −13.1% |
| TCP loopback 256 KB | 7802.7 | 7735.3 | −0.9% |
| TCP loopback 1 MB | 10142.7 | 9422.5 | −7.1% |
| TCP RTT（1000 连接） | avg 23.8 / p50 38.8 / p99 92.3 ms | avg 16.2 / p50 20.4 / p99 23.7 ms | 明显更好 |
| TCP Throughput（10 MB @ 64 KB） | 822.8 MB/s | 743.2 MB/s | −9.7% |
| TCP Conn Stress（1000） | 15,632.7 conn/s | 18,198.9 conn/s | +16.4% |
| TLS Throughput（10 MB @ 64 KB） | 132.6 MB/s | 123.5 MB/s | −6.9% |

TCP loopback 全量（1024 MB）：4 KB 927.9 / 8 KB 1708.4 / 16 KB 2750.2 / 32 KB 3866.7 / 64 KB 4393.9 / 128 KB 5963.9 / 256 KB 7735.3 / 512 KB 8816.6 / 1024 KB 9422.5 MB/s。

## 7. HTTP / HTTP2（回环 keep-alive）

| Bench | 10-14 | 10-17 | Δ |
|---|---:|---:|---:|
| HTTP/1.1（20K req） | 17,963 req/s（55.4 μs） | 15,983 req/s（62.2 μs） | −11.0% |
| HTTP/2 seq（10K, c=1） | 13,533 | 12,095.6 | −10.6% |
| HTTP/2 par8（10K） | 40,785 | 42,166.9 | +3.4% |
| HTTP/2 par64（10K） | **52,325** | 45,795.1 | −12.5% |

## 8. 其他单轮 Bench

| Bench | 10-14 | 10-17 | 备注 |
|---|---|---|---|
| PostJob 扩展（1/2/4/8/16 线程） | 8 线程 734 M/s（3.0×）；16 线程 1546 M（6.3×） | 226 / 423 / 435 / 427 / 446 M/s | 单轮低抽样：复测 16 线程出现 420 / 954 / 1663 M/s |
| Flake ID（8 线程） | 255.3 M ids/s | 211.3 M ids/s | 单轮 |
| StringAppendF | 10.93 M ops/s（1.22× / 1.07×） | 9.65 M ops/s | — |
| OnceCallback / Repeating / BindOnce | 39.0 / 103.6 / 25.4 M/s | 33.8 / 93.2 / 32.3 M/s | 单轮 |
| Parallel Runner 原子递增 | 16 workers 0.3×（竞争，预期） | 同左 | 预期行为 |

---

## 9. 采集口径与建议

1. WSL 侧单轮 bench 方差比 Windows 更大（本轮 `post_job` 16 线程：420 / 954 / 1663 M/s，4×），
   跨版本对比请用多轮中位数；本文件的单轮项只用于"同机同提交"的纵向观察。
2. `run_all_benches.ps1` 目前是 Windows-only（`.exe` + 反斜杠）。若要一条命令覆盖双平台，
   建议把 bench 清单抽成数据文件（名称/参数/轮数），ps1 与 sh 各自只负责执行与日志落盘。
3. 与 10-14 相比，WSL 的 Log 全面变慢（+11~21%）、TCP/TLS/HTTP 略降（−7~−12%），
   而对照组 Raw atomic −11~13%，量级一致 → 判定为机器状态，而非代码回归。

---

## 10. 追加采集（22:35，steady_clock 修复后）

同日第二轮采集，作为本文件的最新数值。代码改动只有两处：

1. `bench/*.cpp` 的计时时钟由 `std::chrono::high_resolution_clock`（libstdc++ 下等价于
   `system_clock`，非单调）改为 `std::chrono::steady_clock` —— 首轮 WSL 采集出现过
   `Total time: -1959880 µs` / `Average time per log: -19.6`（spdlog file 场景）的负样本；
2. `bench/run_all_benches.sh` 报告解析修正（`logs/sec` 正则转义、`File: <path>` 场景归类）。

本轮 `Total time` / `Average time per log` / `Logs per second` 负值 **0 处**。
原始日志：`bench/results/bench_20261017_223501/`

> 注：首轮 WSL 采集（`bench_20261017_221948`）用的是未重编译的旧二进制，其 log_bench 数值作废。

### 10.1 Task scheduling（M/s，10 轮均值）

| Scenario | tracing ON | tracing OFF |
|---|---:|---:|
| Standard PostTask (fast-path) | 2.43 | 2.38 |
| Delayed PostTask (non-fast-path) | 3.11 | 3.41 |
| Multi-threaded PostTask (4 threads) | 2.62 | 2.87 |
| BindOnce + Run (no queue) | 25.78 | 24.38 |
| BindOnce construction | 33.66 | 31.29 |
| Raw atomic loop（对照） | 120.42 | 121.23 |

### 10.2 ThreadPool（M/s，5 轮均值）

| Scenario | M/s |
|---|---:|
| Parallel PostTask (single-thread post) | 3.28 |
| SingleThread — Delayed | 2.40 |
| SingleThread — dedicated-worker 外部投递 | 2.24 |
| SingleThread — MT (4 threads) | 2.01 |
| Standard PostTask (external, global-heap) | 2.00 |
| Multi-threaded PostTask (4 threads, sequenced) | 1.89 |
| Delayed PostTask (non-fast-path) | 1.80 |
| Parallel MT (4 threads) | 1.59 |
| Parallel Worker-Repost (global-heap) | 0.297 |
| Worker-Repost (local-queue fast-path) | 0.225 |

`task_threadpool_parallel_bench`：Parallel PostTask 3.00；Parallel MT 1.52。

### 10.3 Log（NEI，5 轮均值）

| 分组 | μs/log | M logs/s |
|---|---:|---:|
| Memory 各变体（Info/Warn/Error/literal/verbose） | 0.33–0.42 | 2.4–3.1 |
| File 各变体（含 weak / 格式化） | 2.13–2.45 | 0.41–0.47 |

（逐项明细见 `bench_20261017_223501/report.md` 第 5 节，共 27 行；带 `File:` 的场景已正确归类。）

### 10.4 File / Pipe / TCP / TLS / HTTP

| 项目 | 结果 |
|---|---|
| AsyncFile | write 2072.2 MB/s @256 KB；read 4117.1 MB/s @256 KB |
| PipeStream 进程内 | 4K 4298.4 / 16K 6218.4 / 64K 6700.8 / 256K **6962.5** / 1M 6576.2 MB/s |
| PipeStream 跨进程 | P50 35.31 / P95 59.27 µs，26,127 msgs/s |
| TCP loopback（1024 MB） | 4K 966.9 / 16K 2908.1 / 64K 4724.5 / 256K 8390.3 / 1024K **9577.6** MB/s |
| TCP RTT（1000 连接） | avg 23.33 / p50 32.45 / p99 64.60 ms，0 失败 |
| TCP Throughput（10 MB @ 64 KB） | 766.2 MB/s |
| TCP Conn Stress（1000） | 14,410.0 conn/s |
| TLS Throughput（10 MB @ 64 KB） | 128.4 MB/s |
| HTTP/1.1（20K req） | 15,022.2 req/s（avg 66.2 µs） |
| HTTP/2 seq / par8 / par64 | 12,586.6 / 41,287.5 / **48,850.1** req/s |

### 10.5 其他单轮

| Bench | 结果 |
|---|---|
| PostJob 扩展（1/2/4/8/16 线程） | 221.9 / 437.0 / 421.1 / 434.2 / 1104.6 M ops/s |
| Flake ID（8 线程） | 199.9 M ids/s |
| StringAppendF | 9.18 M ops/s |
| OnceCallback / RepeatingCallback / BindOnce | 33.7 / 93.9 / 34.0 M/s |
