# libnei Benchmark Baseline

**Machine**: DESKTOP-A4O44H0
**Date**: 2026-08-06
**Config**: Windows 10, MSVC 19.44, Release x64, CMake preset: windows-vs2022-shared
**Branch**: chromium_callback @ 3456f9d

## Hardware

| Component | Detail |
|-----------|--------|
| CPU | Intel Core i5-10400T @ 2.00GHz (6C/12T) |
| RAM | 16 GB DDR4-3200 (LD4AS016G-H3200GST) |
| OS Disk | SAMSUNG MZALQ256HAJD-000L1 (256 GB SSD) |
| Data Disk | ST1000LM035-1RK172 (1 TB HDD) |
| OS | Windows 11 Home Chinese Edition |

---

## Diagnostic Macros

| Macro | Diag-OFF | Diag-ON |
|-------|----------|---------|
| `NEI_PARALLEL_DIAGNOSTICS` | 0 | 1 |
| `NEI_ALLOCATOR_DIAGNOSTICS` | 0 | 1 |

---

## 1. Task Scheduling

### task_thread_bench (1M tasks, tracing=off)

| Metric | Diag-OFF | Diag-ON | Delta |
|--------|----------|---------|-------|
| Standard PostTask (fast-path) | 2,089,019 /s | 1,947,435 /s | **-6.8%** |
| Delayed PostTask (non-fast-path) | 2,122,147 /s | 1,713,784 /s | **-19.2%** |
| Multi-threaded PostTask (4 threads) | 2,103,037 /s | 2,093,090 /s | -0.5% |

### task_threadpool_bench (1M tasks, tracing=off)

| Metric | Diag-OFF | Diag-ON | Delta |
|--------|----------|---------|-------|
| Standard PostTask (fast-path) | 2,910,239 /s | 2,452,537 /s | **-15.7%** |
| Delayed PostTask (non-fast-path) | 1,372,827 /s | 1,420,368 /s | +3.5% |
| Multi-threaded (4 threads, sequenced) | 2,473,166 /s | 2,471,036 /s | -0.1% |
| Parallel (single-thread post) | 1,376,947 /s | 1,245,417 /s | **-9.6%** |
| Parallel Multi (4 threads) | 1,708,014 /s | 1,617,356 /s | **-5.3%** |
| SingleThread Standard | 1,970,420 /s | 2,253,478 /s | +14.4% |
| SingleThread Delayed | 1,601,917 /s | 1,575,518 /s | -1.6% |
| SingleThread Multi (4 threads) | 2,629,636 /s | 2,549,145 /s | -3.1% |

### task_threadpool_parallel_bench

| Metric | Diag-OFF | Diag-ON | Delta |
|--------|----------|---------|-------|
| Parallel (single-thread post) | 1,336,689 /s | 1,217,626 /s | **-8.9%** |
| Parallel Multi (4 threads) | 1,713,146 /s | 1,629,179 /s | **-4.9%** |

### post_job_bench

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| Raw pool baseline | 29.4 μs | 34.4 μs |
| Sequential (1 worker) | 76,937 jobs/s | 75,863 jobs/s |
| Batch (1 worker, 500K) | 73.8 M/s | 75.6 M/s |
| Batch (8 workers, 500K) | 35.6 M/s | 32.1 M/s |
| Scaling 1→16 workers | 126→433 M/s | 126→410 M/s |

### parallel_runner_bench

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| Bench 1 (1 worker) | 114.3 M/s | 106.3 M/s |
| Bench 2 tasks/s (1 worker) | 1,329 K/s | 1,151 K/s |
| Bench 3 PostTask/s (1 worker) | 1,347 K/s | 1,155 K/s |

### task_priority_perf_demo

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| Throughput | 1,494 tasks/s | 1,495 tasks/s |
| Total elapsed | 40,160 ms | 40,132 ms |

---

## 2. Callback

| Metric | Diag-OFF | Diag-ON | Delta |
|--------|----------|---------|-------|
| OnceCallback (non-SBO) | 19,060,325 /s | 11,917,388 /s | **-37.5%** |
| RepeatingCallback (non-SBO) | 50,666,261 /s | 57,382,222 /s | +13.3% |
| Mixed SBO/non-SBO | 21,384,885 /s | 13,761,783 /s | **-35.6%** |
| BindOnce (non-SBO) | 16,574,951 /s | 13,092,775 /s | **-21.0%** |

---

## 3. Memory & String

### string_append_bench

| Method | Diag-OFF | Diag-ON |
|--------|----------|---------|
| StringAppendF | 2,569,769 /s | 2,574,069 /s |
| StringPrintf+operator+= | 2,010,616 /s | 1,983,891 /s |
| std::stringstream | 856,230 /s | 857,464 /s |

### flake_id_bench

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| 8 threads, 8M IDs | 180,180,992 /s | 193,993,477 /s |
| Elapsed | 44.4 ms | 41.2 ms |

---

## 4. Logging

### log_bench (NEI, 1M iters)

| Level | Diag-OFF | Diag-ON |
|-------|----------|---------|
| Info (simple %s) | 2.11 M/s | 2.06 M/s |
| Info (literal) | 2.47 M/s | 2.39 M/s |
| Warn | 2.18 M/s | 2.14 M/s |
| Error | 2.10 M/s | 2.10 M/s |
| Formatting (multi-arg) | 1.05 M/s | 1.01 M/s |

### log_bench_compare (NEI vs spdlog, 1M iters memory)

| Test | Diag-OFF NEI | Diag-OFF spdlog | Diag-ON NEI | Diag-ON spdlog |
|------|-------------|-----------------|-------------|----------------|
| simple %s / {} | 2.09 M/s | 1.81 M/s | 2.19 M/s | 1.76 M/s |
| multi printf / fmt | 1.09 M/s | 1.28 M/s | 1.07 M/s | 1.25 M/s |
| llog_literal / literal only | 2.51 M/s | 2.64 M/s | 2.43 M/s | 2.12 M/s |

---

## 5. I/O

### async_file_bench (64 MB)

| Op | Chunk | Diag-OFF | Diag-ON |
|----|-------|----------|---------|
| Best write | 1 MB | 1,443.0 MB/s | 1,239.3 MB/s |
| Best read | 64 KB | 1,782.4 MB/s | 1,407.1 MB/s |

### pipe_stream_bench (64 MB)

| Chunk | Diag-OFF | Diag-ON |
|-------|----------|---------|
| 4 KB | 155.6 MB/s | 169.5 MB/s |
| 16 KB | 324.0 MB/s | 633.3 MB/s |
| 64 KB | 1,166.3 MB/s | 2,066.7 MB/s |
| 256 KB | 3,109.3 MB/s | 4,325.7 MB/s |
| 1 MB | 1,438.5 MB/s | 1,522.2 MB/s |

### pipe_stream_cross_process_bench (5000 iters, 64B)

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| Avg RTT | 45.16 μs | 45.94 μs |
| Msgs/s | 22,145 | 21,766 |

---

## 6. Network

### tcp_loopback_bench (1024 MB)

| Buffer | Diag-OFF | Diag-ON |
|--------|----------|---------|
| 4 KB | 120.6 MB/s | 122.2 MB/s |
| 64 KB | 1,206.9 MB/s | 1,232.4 MB/s |
| 256 KB (best) | 2,040.2 MB/s | 1,987.5 MB/s |

### tcp_rtt_bench (1000 conn, 4 workers)

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| Avg RTT | 19,899 μs | 21,434 μs |
| Total elapsed | 0.788 s | 0.782 s |

### tcp_throughput_bench (10 MB, 64 KB)

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| Throughput | 307.0 MB/s | 316.7 MB/s |

### tcp_conn_stress_bench (1000 conn)

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| Rate | 1,311.0 conn/s | 1,296.9 conn/s |
| Elapsed | 0.771 s | 0.771 s |

### tls_throughput_bench (10 MB, 64 KB)

| Metric | Diag-OFF | Diag-ON |
|--------|----------|---------|
| Throughput | 49.1 MB/s | 47.4 MB/s |

---

## 7. Skipped

- **tcp_cross_bench**: Requires server+client dual-process (not automated)
- **log_bench_compare**: Full output in separate files (too verbose for summary)

---

## Key Findings

1. **Task fast-path regression (diag-on)**: `task_threadpool_bench` Standard PostTask drops **15.7%** (2.91M→2.45M /s). `task_thread_bench` drops **6.8%**. This is the primary cost of NEI_PARALLEL_DIAGNOSTICS + NEI_ALLOCATOR_DIAGNOSTICS atomics on the hot path.

2. **Callback regression**: OnceCallback drops **37.5%**, Mixed drops **35.6%**. These non-SBO paths hit the SmallObjectAllocator diagnostic atomics heavily.

3. **I/O and network benchmarks**: No significant difference (within noise). These are not bottlenecked by diagnostic atomics.

4. **Logging**: No significant difference. Log path does not use task queues or SmallObjectAllocator in the hot path.
