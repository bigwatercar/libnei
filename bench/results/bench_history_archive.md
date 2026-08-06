# Bench History Archive

Historical benchmark results consolidated from individual reports. Superseded by
the `baseline_*.md` files. Kept for trend reference.

---

## A. task_thread_bench — 2026-05-16

Source: `task_thread_benchmark.md`
Platform: Windows (Ultra 9 185H?), build: windows-vs2022-release-shared

TaskTracing ON, 30 rounds × 1M tasks:

| Stat | total_throughput (tasks/s) |
|------|---------------------------|
| Mean | 6,938,273 |
| Min | 5,869,957 |
| Max | 7,534,376 |
| StdDev | 361,715 |
| P50 | 6,961,074 |
| P95 | 7,360,843 |

---

## B. async_file_bench — 2026-06-13

Source: `async_file_benchmark.md`
Machine: Ultra 9 185H, Windows 11 + WSL2 (Ubuntu)

64 MB per run, IOCP (Win) vs pread/pwrite+thread (Linux).

| Op | Chunk | Windows (MB/s) | WSL2 (MB/s) |
|----|-------|---------------|-------------|
| write | 4 KB | 528.2 | — |
| write | 16 KB | 1,414.0 | — |
| write | 64 KB | 2,295.6 | 1,509.5 |
| write | 256 KB | 3,087.0 | 1,572.2 |
| write | 1 MB | 2,763.6 | 1,604.2 |
| write | 4 MB | 2,822.1 | 1,591.4 |
| read | 4 KB | 1,068.2 | — |
| read | 16 KB | 2,586.2 | — |
| read | 64 KB | 3,408.6 | 4,422.6 |
| read | 256 KB | 3,404.1 | 3,111.5 |
| read | 1 MB | 3,778.5 | 3,788.3 |
| read | 4 MB | 3,927.8 | 3,321.4 |

---

## C. Log Module — 2026-06-24

Source: `log_benchmark.md`
Machine: Ultra 9 185H, 32GB DDR5, NVMe SSD, Windows 11

### Memory (1M iters)

| Scene | E2E μs/log | E2E logs/s |
|-------|-----------|-----------|
| Log Info | 0.340 | 2,942,480 |
| Log Warn | 0.351 | 2,846,420 |
| Log Error | 0.342 | 2,925,140 |
| Formatting | 0.560 | 1,784,800 |
| Info (literal) | 0.301 | 3,325,730 |
| Verbose | 0.359 | 2,785,570 |
| Verbose (literal) | 0.326 | 3,071,150 |

### File (100K iters)

| Scene | E2E μs/log | E2E logs/s | File Size |
|-------|-----------|-----------|----------|
| File Info | 0.409 | 2,444,270 | 8.8 MB |
| File Warn | 0.432 | 2,314,810 | 8.8 MB |
| File Error | 0.456 | 2,193,940 | 8.8 MB |
| Formatting | 0.480 | 2,081,560 | 10.3 MB |
| Info (literal) | 0.464 | 2,152,950 | 9.3 MB |
| Verbose | 0.447 | 2,237,490 | 9.4 MB |
| Verbose (literal) | 0.407 | 2,457,120 | 9.1 MB |

### NEI vs spdlog (memory, 1M iters)

| Test | NEI | spdlog |
|------|-----|--------|
| simple %s / {} | 2.89 M/s | 2.04 M/s |
| multi printf / fmt | 1.81 M/s | 1.75 M/s |
| literal | 3.18 M/s | 3.56 M/s |

---

## D. Log Baseline — 2026-07-31

Source: `log_baseline_20260731.txt`
Machine: Ultra 9 185H, Windows 11, `post_job_implement` branch (16d50f2)

> Raw `log_bench` + `log_bench_compare` output. See original file for full
> phase-breakdown and spdlog comparison tables.
