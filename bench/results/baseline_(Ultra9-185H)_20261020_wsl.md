# libnei — Benchmark Baseline 2026-10-20 (WSL2)

**Machine**: THINKBOOK14PLUS (Intel Ultra 9 185H, 22 cores)，WSL2 Ubuntu-24.04（内核 6.18.33.2-microsoft-standard-WSL2），GCC Release shared
**Build**: `~/libnei_build/wsl-release-shared`（源码 `/mnt/c/.../libnei-src`），宿主电源计划 = 性能
**Commit**: `20f860f`（与同日 Windows 基线 `baseline_(Ultra9-185H)_20261020_windows.md` 同提交）
**采集条件**: 20:18:57–20:26:22，WSL 实例启动约 1 分钟后开跑（`uptime` 起始 loadavg 0.79），期间无并发构建。
**脚本**: `bench/run_all_benches.sh --bin-dir ~/libnei_build/wsl-release-shared/bench --log-dir bench/results`
（与 `run_all_benches.ps1` 共用 `bench/benches.tsv`，参数与轮数完全一致）→ 原始日志 `bench/results/bench_20261020_201857/`
**对照基线**: `baseline_(Ultra9-185H)_20261017_wsl.md` §11（同机同配置、电源计划=性能的那一轮）
**A/B 对照**: 本轮 A/B 在 Windows 侧进行（见 Windows 基线第 8 节），WSL 侧未做交替对照。

---

## 0. 先读结论

1. **任务调度/线程池整体比 10-17 快 0–11%**，与对照 `Raw atomic`（+0~5%）同量级 → 本轮改动无回归，
   本机状态略好于上次。
2. **WSL 的文件系统行为与上次完全不同**：本次 file sink 每行 2.9–3.1 μs（10-17 为 0.31–0.34 μs，约 10×），
   spdlog 同场景由 0.32 → 7.28 μs；strict sync flush 由 0.75–0.81 μs 跳到 **611–625 μs**。
   两侧同步变化 → 是 WSL 虚拟盘/写缓存状态（实例刚重启），**这两组数值不可与 10-17 直接对比**，
   但同一轮内 NEI vs spdlog 的相对比较仍然有效。
3. TCP/HTTP 全面好于 10-17（loopback 1 MB +18.7%、throughput +50%、HTTP/1.1 +13%），与 Windows 侧
   同日出现的 TCP 下降方向相反 → 进一步说明是宿主网络栈状态，而非库代码。

---

## 1. Task scheduling — task_thread_bench（10 轮均值，M/s）

括号内为该 10 轮 min–max。

| Scenario | 10-17 ON | 10-20 ON | Δ | 10-17 OFF | 10-20 OFF | Δ |
|---|---:|---:|---:|---:|---:|---:|
| Standard PostTask (fast-path) | 2.39 | **2.60** (2.39–2.89) | +8.8% | 2.46 | **2.74** (2.53–2.83) | +11.4% |
| Delayed PostTask (non-fast-path) | 3.01 | **3.14** (2.38–3.77) | +4.3% | 3.45 | **3.57** (2.95–3.99) | +3.5% |
| Multi-threaded PostTask (4 threads) | 2.54 | **2.78** (2.53–3.03) | +9.4% | 2.86 | **2.99** (2.68–3.13) | +4.5% |
| BindOnce + Run (no queue) | 24.68 | **26.11** (21.50–28.53) | +5.8% | 25.60 | **26.24** (22.64–28.99) | +2.5% |
| BindOnce construction | 31.43 | **32.63** (21.16–37.16) | +3.8% | 33.60 | **33.83** (29.22–37.46) | +0.7% |
| Raw atomic loop（对照，无库代码） | 117.60 | **116.42** (81.22–137.47) | −1.0% | 118.43 | **124.52** (77.21–134.44) | +5.1% |

所有分项与对照组的偏移同向同量级，判定为本机状态差异，不是回归。

## 2. Task scheduling — task_threadpool_bench（1M tasks，5 轮均值，M/s）

| Scenario | 10-17 | 10-20 | Δ |
|---|---:|---:|---:|
| SingleThread — dedicated-worker 外部投递 | 2.46 | **2.76** (2.52–3.03) | +12.2% |
| Parallel PostTask (single-thread post) | 2.91 | **2.96** (2.53–3.16) | +1.7% |
| SingleThread — Delayed | 2.39 | **2.45** (2.23–2.68) | +2.5% |
| Standard PostTask (external, global-heap) | 2.01 | **2.13** (2.04–2.26) | +6.0% |
| Multi-threaded PostTask (4 threads, sequenced) | 1.90 | **2.06** (1.92–2.18) | +8.2% |
| SingleThread — MT (4 threads) | 1.93 | **2.01** (1.92–2.07) | +4.1% |
| Delayed PostTask (non-fast-path) | 1.77 | **1.83** (1.75–1.92) | +3.4% |
| Parallel MT (4 threads) | 1.57 | **1.52** (1.42–1.60) | −3.3% |
| Parallel Worker-Repost (global-heap) | 0.300 | **0.324** (0.311–0.342) | +8.0% |
| Worker-Repost (local-queue fast-path) | 0.220 | **0.230** (0.218–0.240) | +4.5% |

`task_threadpool_parallel_bench`（5 轮）：Parallel PostTask **3.00**（2.47–3.87）；Parallel MT **1.45**（1.42–1.48）。

> WSL 的 ThreadPool 各项仍明显低于 Windows（约 2.1 vs 4.7 M/s），这是 WSL2 futex 唤醒 + 调度器开销，
> 与 10-14/10-17 基线一致，不是代码差异。

## 3. Log（NEI，5 轮均值，μs/log）

| 分组 | 10-20 μs/log | 10-20 M logs/s | 10-17（参考） |
|---|---:|---:|---:|
| Memory：Log Info | 0.344 | 2.92 | 0.335–0.439 |
| Memory：Log Warn / Error | 0.353 / 0.340 | 2.85 / 2.94 | 同上 |
| Memory：Log with Formatting | 0.420 | 2.39 | 同上 |
| Memory：literal / verbose 各变体 | 0.325–0.400 | 2.36–3.09 | 同上 |
| File：各变体 | **2.92–3.13** | 0.32–0.34 | 2.39–2.59 |

> File sink 本轮比 10-17 慢约 20–30%，与第 5 节的文件系统状态变化同源（同一轮内 spdlog 从
> 0.32 跳到 7.28 μs，幅度更大）。

## 4. Log Compare（NEI vs spdlog，5 轮均值，μs/log）

| 场景 | NEI | spdlog | 结论 |
|---|---:|---:|---|
| memory simple（`%s` vs `{}`） | 0.308 | 0.486 | NEI 快 ~37% |
| memory literal（llog / vlog） | 0.299 / 0.283 | 0.445 | NEI 快 ~33–36% |
| memory multi | 0.409 | 0.692 | NEI 快 ~41% |
| memory `{fmt}` literal | 0.320（simple）/ 0.436（multi） | — | — |
| memory multi（fmt_plan cache miss） | 0.403 | — | — |
| file async simple / multi | **2.858 / 3.069** | **7.284 / 8.777** | NEI 快 ~60–65%（本轮文件系统状态下差距放大） |
| file async literal（llog / vlog） | 2.763 / 2.838 | — | — |
| per-call flush（autoflush，1 ms 定时器） | 2.774–3.091 | 0.173–0.184（spdlog async logger + flush 请求） | 语义不同，不可直接比较 |
| strict sync flush simple / multi | 611.0 / 624.6 | 622.2 / 621.3 | **相当**（10-17：NEI 0.75 / spdlog 0.55，当时未真正落盘） |

> 本轮 strict sync flush 才第一次体现出真实 fsync 成本（~0.6 ms/log），因此 NEI 与 spdlog 从
> “亚微秒级、无从比较” 变成 “基本持平”。与 Windows（NEI 17–18 μs vs spdlog 13.5–13.7 μs）仍然
> 差两个数量级，说明 WSL 虚拟盘 fsync 代价远高于 Windows。

## 5. File / Pipe I/O（单轮）

| 项目 | 10-17 | 10-20 | Δ |
|---|---:|---:|---:|
| AsyncFile write（最佳块） | 2113.5 MB/s @1 MB | **1734.9 MB/s @1 MB** | −17.9% |
| AsyncFile read（最佳块） | 5629.3 MB/s @64 KB | **5138.9 MB/s @64 KB** | −8.7% |

PipeStream 进程内（64 MB，MB/s）：

| Chunk | 4 KB | 16 KB | 64 KB | 256 KB | 1 MB |
|---|---:|---:|---:|---:|---:|
| 10-17 | 4266.9 | 6501.4 | 6953.4 | 7032.9 | 6815.0 |
| 10-20 | 3769.3 | 5780.8 | **6961.0** | 6113.8 | 3568.6 |

PipeStream 跨进程（64 B × 5000）：P50 35.68 μs / P95 81.31 / avg 41.20 / max 257.53，**24,274 msgs/s**
（10-17：P50 35.23 / P95 59.81 / 26,716 msgs/s；P95 抖动大是 WSL 调度特性）。

## 6. TCP / TLS

| 项目 | 10-17 | 10-20 | Δ |
|---|---:|---:|---:|
| TCP loopback 4 KB | 927.9 | **836.1 MB/s** | −9.9% |
| TCP loopback 64 KB | 4393.9 | **4916.4 MB/s** | +11.9% |
| TCP loopback 256 KB | 7735.3（§11：6541.5） | **8645.7 MB/s** | +11.8% / +32.2% |
| TCP loopback 1 MB | 9422.5（§11：8110.1） | **9630.7 MB/s** | +2.2% / +18.7% |
| TCP RTT（1000 连接） | avg 16.90 / p50 20.65 / p99 49.29 ms | avg 14.91 / p50 19.70 / p99 49.53 ms，0 失败 | 略好 |
| TCP Throughput（10 MB @ 64 KB） | 743.2（§11：528.0） | **794.1 MB/s** | +6.9% / +50.4% |
| TLS Throughput（10 MB @ 64 KB） | 123.5（§11：116.1） | **124.4 MB/s** | +0.7% / +7.1% |
| TCP Conn Stress（1000） | 18,198.9（§11：12,240.2） | **14,518.3 conn/s** | −20.2% / +18.6% |

TCP loopback 全量（1024 MB）：4 KB 836.1 / 8 KB 1580.5 / 16 KB 2838.0 / 32 KB 4200.0 / 64 KB 4916.4 /
128 KB 6785.2 / 256 KB 8645.7 / 512 KB 8436.5 / 1024 KB 9630.7 MB/s。

> **观测项（非本轮引入）**：`tcp_conn_stress` 报 `Server accepts: 543 / Client done: 1000 (failures: 0)`
> （10-17 同类：`accept 1 例失败`）。客户端全部完成而服务端 accept 计数偏低，与 Windows 侧
> （1000 accept）不一致，值得单独复核 bench 侧计数口径或 accept 折叠逻辑。

## 7. HTTP / HTTP2（回环 keep-alive）

| Bench | 10-17 | 10-20 | Δ |
|---|---:|---:|---:|
| HTTP/1.1（20K req） | 15,983（§11：13,839.9） | **15,646.0 req/s（63.6 μs）** | −2.1% / +13.0% |
| HTTP/2 seq（10K, c=1） | 12,095.6（§11：12,040.9） | **11,462.6**（mean RTT 87.1 μs） | −5.2% / −4.8% |
| HTTP/2 par8（10K） | 42,166.9 | **40,597.4** | −3.7% |
| HTTP/2 par64（10K） | 45,795.1（§11：44,998.4） | **46,940.2** | +2.5% / +4.3% |

## 8. 其他单轮 Bench

| Bench | 10-17 | 10-20 | 备注 |
|---|---|---|---|
| PostJob Per-worker（1/2/4/8/16） | 211.9 / — / — / 1147.5 / 1353.4 M ops/s | 221.2 / 432.8 / 442.5 / 992.3 / **1131.5** M ops/s（5.1×） | 单轮 |
| PostJob 批量（500K ops） | — | PostJob 1w 26.6 / SeqRunner 2.0 / PostJob 8w 23.1 M/s | SeqRunner 明显低于 Windows（4.1 M/s） |
| Flake ID（8 线程） | 205.6 M ids/s | **215.5 M ids/s** | 单轮 |
| StringAppendF | 9.18 M ops/s | **8.33 M ops/s（1.20× / 1.08×）** | WSL 上 vs stringstream 优势很小 |
| OnceCallback / RepeatingCallback / BindOnce | 33.7 / 93.9 / 34.0 M/s | **35.1 / 85.9 / 34.4 M/s** | 单轮 |
| Parallel Runner 原子递增（1→16） | 16 workers 0.2–0.3× | 231.8 / 74.8 / 69.0 / 59.4 / 52.6 M/s | 竞争，符合预期 |
| Parallel Runner 小任务 / PostTask（1→16） | — | 324→274 K/s / 314→262 K/s | — |
| Task priority（60K / 3 优先级 / 4 workers） | — | 份额 **33.3% / 33.3% / 33.3%**，全局平均排队 1250.5 ms | 公平性保持 |

## 9. 采集口径与建议

1. WSL 侧单轮项方差比 Windows 更大，跨版本对比请用多轮中位数；本文件的单轮项只用于同机纵向观察。
2. **本轮 WSL 文件系统状态异常**（file sink 慢 10×、strict flush 慢 800×），这两组只能同轮内比较，
   下轮需在实例稳定运行一段时间后复核。
3. 与 Windows 同日的方向差异（Windows TCP ↓、WSL TCP ↑）说明网络相关单轮项受宿主/虚拟网卡状态主导；
   需要精确版本对比时请用交替 A/B（Windows 侧已有现成流程，见 Windows 基线第 8 节）。
4. `tcp_conn_stress` 的服务端 accept 计数在 WSL 上偏低（543/1000），建议单独复核（bench 计数口径
   或 accept 折叠逻辑），不影响本轮基线结论。
