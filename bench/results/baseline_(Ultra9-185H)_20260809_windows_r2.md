# NEI 全量 Benchmark 基线报告（Windows · 本轮）

**日期**: 2026-08-09
**分支**: `dev` @ HEAD `2cafb78`
**构建**: Visual Studio 2022, `windows-vs2022-shared` (Release x64, shared)
**机器**: Intel Core Ultra 9 185H (6P+8E+2LPE, 16C/22T), Windows 11
**原始日志**: `bench/results/bench_20260809_150852/`（gitignored）

> 本轮相对当日早前基线（`baseline_(Ultra9-185H)_20260809_windows.md` @ `7edd09e`）的变更：
> `ffd3642` PostJob work-stealing joiner spawn-exit 风暴/溢出修复、
> `2cafb78` PooledTaskSource::Shutdown condvar 丢失唤醒修复。
> 任务调度口径：1M tasks, tracing=off（task_thread）/ 1M（threadpool）。

## 1. Task Thread（Post throughput，10×1M）

| Scenario | 均值 /s |
|---|---:|
| Raw atomic loop | 132,202,144 |
| BindOnce construction | 38,424,688 |
| BindOnce + Run | 29,172,270 |
| Standard PostTask（fast-path） | 3,816,478 |
| Delayed PostTask（non-fast-path） | 3,790,183 |
| Multi-threaded PostTask（4 线程） | 2,396,240 |

## 2. Task ThreadPool（5×1M）

| Scenario | 均值 /s |
|---|---:|
| Parallel PostTask（单线程投递） | 4,574,905 |
| Standard PostTask（global-heap） | 5,303,419 |
| SingleThread Standard（dedicated-worker） | 5,020,601 |
| SingleThread Delayed | 3,551,397 |
| Delayed PostTask | 3,432,104 |
| Multi-threaded（sequenced） | 3,085,121 |
| SingleThread Multi-threaded（4 线程） | 2,882,399 |
| Parallel Multi-threaded（4 线程） | 2,744,077 |
| Parallel Worker-Repost（global-heap） | 515,052 |
| Worker-Repost（local-queue） | 442,112 |

## 3. Task ThreadPool Parallel（5×1M）

| Scenario | 均值 /s |
|---|---:|
| Parallel PostTask（单线程投递） | 4,502,327 |
| Parallel Multi-threaded（4 线程） | 2,737,125 |

## 4. Log Bench（E2E logs/s，5 轮均值）

| Benchmark | 均值 /s |
|---|---:|
| Log Info | 3,114,338 |
| Log Warn | 3,111,142 |
| Log Error | 3,046,712 |
| Log with Formatting | 1,825,040 |
| Log Info (literal) | 2,876,212 |
| Log Verbose | 3,153,004 |
| Log Verbose (literal) | 3,019,398 |

## 5. 单次 Bench

| Bench | 结果 |
|---|---:|
| PostJob Bench1（50K jobs） | 275,854 jobs/s |
| PostJob Bench3 w=16 | 1,293 M/s（5.2× vs 1 worker） |
| Flake ID（8 线程×1M） | 273,260,441 ids/s |
| StringAppendF / StringPrintf+ / stringstream | 5.16 / 4.16 / 1.88 M ops/s |
| Callback Once / Repeating | 38.5 / 96.9 M/s |
| AsyncFile write best / read best | 2,874 / 3,940 MB/s（256K / 64K） |
| PipeStream best | 6,433 MB/s（256K） |
| PipeStream 跨进程（64B） | 66,168 msgs/s（p50 14.5µs） |
| TCP Loopback best | 3,598 MB/s（128K） |
| TCP RTT avg / p99 | 10,111 / 14,124 µs |
| TCP Throughput | 256.9 MB/s |
| TCP Conn Stress | 1,667 conn/s（accepts 1000，failures 1） |
| TLS Throughput | 93.7 MB/s |
| Parallel Runner w=1 | 257.7 M/s |
| Task Priority（6 万任务） | 三优先级均分 ✅（Queue 1251/1250/1251ms） |
