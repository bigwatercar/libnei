# NEI 全量 Benchmark 基线报告（WSL2 · 本轮）

**日期**: 2026-08-09
**分支**: `dev` @ HEAD `2cafb78`
**构建**: WSL2 Ubuntu（GCC 13, Release x64, shared）, `linux-gcc-release-shared`
**机器**: Intel Core Ultra 9 185H（WSL2, 同一台 THINKBOOK14PLUS）
**原始日志**: `bench/results/wsl_20260809/`

> 与 Windows 基线（`baseline_(Ultra9-185H)_20260809_windows_r2.md`）同轮采集。
> 任务调度口径：1M tasks, tracing=off（task_thread）/ 1M（threadpool）。

## 1. Task Thread（Post throughput，10×1M）

| Scenario | 均值 /s |
|---|---:|
| Raw atomic loop | 132,049,140 |
| BindOnce construction | 37,034,115 |
| BindOnce + Run | 28,825,010 |
| Standard PostTask（fast-path） | 3,673,927 |
| Delayed PostTask（non-fast-path） | 4,154,023 |
| Multi-threaded PostTask（4 线程） | 3,191,092 |

## 2. Task ThreadPool（5×1M）

| Scenario | 均值 /s |
|---|---:|
| Parallel PostTask（单线程投递） | 5,441,110 |
| Standard PostTask（global-heap） | 3,669,621 |
| SingleThread Standard（dedicated-worker） | 3,358,210 |
| SingleThread Delayed | 2,609,306 |
| Delayed PostTask | 2,266,454 |
| Multi-threaded（sequenced） | 2,395,657 |
| SingleThread Multi-threaded（4 线程） | 2,427,896 |
| Parallel Multi-threaded（4 线程） | 2,119,335 |
| Parallel Worker-Repost（global-heap） | 490,928 |
| Worker-Repost（local-queue） | 124,881 |

## 3. Task ThreadPool Parallel（5×1M）

| Scenario | 均值 /s |
|---|---:|
| Parallel PostTask（单线程投递） | 5,282,064 |
| Parallel Multi-threaded（4 线程） | 2,156,629 |

## 4. Log Bench（E2E logs/s，5 轮均值）

| Benchmark | 均值 /s |
|---|---:|
| Log Info | 3,391,342 |
| Log Warn | 3,411,878 |
| Log Error | 3,396,886 |
| Log with Formatting | 2,828,356 |
| Log Info (literal) | 3,534,586 |
| Log Verbose | 3,310,222 |
| Log Verbose (literal) | 3,525,030 |

## 5. 单次 Bench

| Bench | 结果 |
|---|---:|
| PostJob Bench1（50K jobs） | 29,413 jobs/s ⚠️（Windows 275K） |
| PostJob Bench3 w=16 | 798 M/s（3.1× vs 1 worker）⚠️（Windows 1,293 M/s） |
| Flake ID（8 线程×1M） | 256,215,410 ids/s |
| StringAppendF / StringPrintf+ / stringstream | 11.38 / 9.59 / 11.33 M ops/s |
| Callback Once / Repeating | 37.4 / 105.6 M/s |
| AsyncFile write best / read best | 1,788 / 5,855 MB/s（1M / 64K） |
| PipeStream best | 6,469 MB/s（64K） |
| PipeStream 跨进程（64B） | 30,589 msgs/s（p50 30.2µs） |
| TCP Loopback best | 10,670 MB/s（1M） |
| TCP RTT avg / p99 | 10,466 / 11,505 µs |
| TCP Throughput | 880.2 MB/s |
| TCP Conn Stress | 14,437 conn/s（accepts 1000，failures 1） |
| TLS Throughput | 123.5 MB/s |
| Parallel Runner w=1 | 256.7 M/s |
| Task Priority（6 万任务） | 三优先级均分 ✅（Queue 1251/1250/1251ms） |

## 6. 双平台差异速查（WSL vs Windows）

| Bench | Win | WSL | 备注 |
|---|---:|---:|---|
| Worker-Repost（local-queue） | 0.44 M/s | 0.12 M/s | ⚠️ WSL 明显掉速 |
| PostJob Bench1 | 275K jobs/s | 29K jobs/s | ⚠️ 已知 joiner 空转问题（bench 特有） |
| PostJob Bench3 w=16 | 1,293 M/s | 798 M/s | ⚠️ 同上 |
| TCP Loopback / Conn Stress | 3.6K MB/s / 1.7K conn/s | 10.7K MB/s / 14.4K conn/s | OS TCP 栈差异（已知） |
| AsyncFile write | 2,874 MB/s | 1,788 MB/s | Windows 更快 |
| AsyncFile read | 3,940 MB/s | 5,855 MB/s | WSL 更快 |
