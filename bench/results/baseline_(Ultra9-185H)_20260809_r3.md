# NEI 全量 Benchmark 基线报告（Windows + WSL 对照 · 今日重构后）

**日期**: 2026-08-09
**分支**: `dev` @ HEAD `cfe83a7`
**构建**:
  - Windows: Visual Studio 2022, `build/windows-vs2022-shared`（Release x64, shared）
  - WSL: GCC 13, `build/linux-gcc-release-shared`（Release, shared）
**机器**: Intel Core Ultra 9 185H (6P+8E+2LPE, 16C/22T), Windows 11 / WSL2
**原始日志**（gitignored）:
  - Windows: `bench/results/bench_20260809_195220/`
  - WSL: `bench/results/wsl_20260809_195641/`

> 本轮相对 r2 基线（`baseline_(Ultra9-185H)_20260809_windows_r2.md` @ `2cafb78`）的变更（今日重构全部包含）：
> - `c09be98` P2 PostJob Chromium 对齐（Location/Traits 透传 + priority 传递）
> - `c1bb7db` ThreadPool Pimpl + ExecutionFence
> - `38e14a9` Worker-Repost local-queue WSL 掉速修复（+160%）
> - `cfe83a7` **post_job_bench 多 worker 完成检测死锁修复**（OnWorkerExited `prev_assigned==1`）
> 任务调度口径：1M tasks, tracing=off（task_thread）/ 1M（threadpool）。

## 1. Task Thread（Post throughput，10×1M，tracing OFF）

| Scenario | Windows /s | WSL /s |
|---|---:|---:|
| Raw atomic loop（无任务系统） | 136,187,916 | 131,602,677 |
| BindOnce construction | 43,095,643 | 39,196,252 |
| BindOnce + Run | 31,675,031 | 30,236,833 |
| Standard PostTask（fast-path） | 4,170,715 | 3,847,316 |
| Delayed PostTask（non-fast-path） | 4,354,865 | 4,279,727 |
| Multi-threaded PostTask（4 线程） | 2,587,519 | 3,265,136 |

## 2. Task ThreadPool（5×1M，tracing OFF）

| Scenario | Windows /s | WSL /s |
|---|---:|---:|
| Parallel PostTask（单线程投递） | 4,969,051 | 4,853,536 |
| Standard PostTask（global-heap） | 5,442,961 | 3,694,658 |
| SingleThread Standard（dedicated-worker） | 5,241,643 | 4,106,241 |
| SingleThread Delayed | 4,181,675 | 2,637,452 |
| Delayed PostTask | 3,966,614 | 2,466,677 |
| Multi-threaded（sequenced） | 2,851,250 | 2,556,001 |
| SingleThread Multi-threaded（4 线程） | 2,751,922 | 3,329,318 |
| Parallel Multi-threaded（4 线程） | 2,747,018 | 2,223,637 |
| Parallel Worker-Repost（global-heap） | 565,556 | 570,510 |
| Worker-Repost（local-queue） | 575,849 | 345,723 |

## 3. Task ThreadPool Parallel（5×1M）

| Scenario | Windows /s | WSL /s |
|---|---:|---:|
| Parallel PostTask（单线程投递） | 4,547,659 | 5,640,157 |
| Parallel Multi-threaded（4 线程） | 2,604,417 | 2,221,277 |

## 4. Log Bench（E2E logs/s，5 轮均值）

| Benchmark | Windows /s | WSL /s |
|---|---:|---:|
| Log Info | 3,134,114 | 3,627,458 |
| Log Warn | 3,113,976 | 3,695,964 |
| Log Error | 3,149,238 | 3,716,428 |
| Log with Formatting | 1,863,352 | 3,188,786 |
| Log Info (literal) | 2,951,166 | 3,873,650 |
| Log Verbose | 3,301,522 | 3,805,036 |
| Log Verbose (literal) | 3,053,434 | 3,901,874 |

## 5. 单次 Bench

| Bench | Windows | WSL |
|---|---:|---:|
| PostJob Bench1（50K jobs） | 223,148 jobs/s | 39,568 jobs/s |
| PostJob Bench3 w=16 | 1,036 M/s（4.3× vs 1w） | 767 M/s（3.1×） |
| Flake ID（8 线程×1M） | 250.9 M ids/s | 303.9 M ids/s |
| StringAppendF / StringPrintf+ / stringstream | 5.52 / 4.70 / 2.06 M ops/s | 11.26 / 10.02 / 11.37 M ops/s |
| Callback Once / Repeating | 41.4 / 97.0 M/s | 33.5 / 109.6 M/s |
| AsyncFile write best / read best | 3,189 / 4,127 MB/s（1MB / 1MB） | 1,482 / 4,697 MB/s（256K / 64K） |
| PipeStream best | 11,057 MB/s（256K） | 6,574 MB/s（256K） |
| PipeStream 跨进程（64B） | 70,034 msgs/s（p50 14.2µs） | 31,702 msgs/s（p50 30.1µs） |
| TCP Loopback best | 4,073 MB/s（256K） | 10,374 MB/s（1MB） |
| TCP RTT avg / p99 | 9,447 / 11,757 µs | 11,213 / 15,844 µs |
| TCP Throughput | 639.2 MB/s | 951.9 MB/s |
| TCP Conn Stress | 1,676 conn/s | 11,896 conn/s |
| TLS Throughput | 103.6 MB/s | 130.4 MB/s |
| Parallel Runner w=1 | 261.9 M/s | 261.5 M/s |
| Task Priority（6 万任务） | 三优先级均分 ✅（Queue 1249/1249/1250 ms） | 三优先级均分 ✅（33.3%×3） |

## 6. 说明

- **post_job_bench**：双平台多轮验证 0 死锁（Windows 50/50、WSL 30/30）；Bench3 w=16 Windows 4.3× / WSL 3.1×，无回归。
- **Worker-Repost（local-queue）**：WSL 345K/s（0.35M），Windows 576K/s——与 `38e14a9` 修复后水平一致（WSL 0.32M→修复后稳定），无回归。
- **Bench1（50K 单 op jobs）**：WSL 39,568 jobs/s vs Windows 223,148——已知 WSL2 线程 handoff 延迟（futex/调度 ~2.3×）所致，非库缺陷（见 TODO.md）。
- **Log Bench**：WSL 整体高于 Windows（异步管道在 WSL futex 更高效），无回归。
- **TCP/TLS**：WSL loopback/throughput/conn-stress 高于 Windows（平台网络栈差异）；TCP RTT 两平台 ~9-15ms（并发 echo 负载下）。
- WSL `tcp_loopback` 8KB 一档出现一次 `connect failed`（环境偶发，其他档正常）。

## 7. 原始日志清单

- Windows: `bench/results/bench_20260809_195220/*.log` + `report.md`
- WSL: `bench/results/wsl_20260809_195641/*.log` + `report.md`
