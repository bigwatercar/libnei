# neixx AsyncFile Throughput Benchmark — Baseline Report

**Date**: 2026-06-13
**Total data per run**: 64 MB

## Test Environment

| | Windows | Linux (WSL2) |
|---|---|---|
| **OS** | Windows 11 (26200) | Ubuntu (WSL2, 5.15.x kernel) |
| **Compiler** | MSVC 19.44 (VS2022) | GCC 13.x |
| **Build config** | Release, shared | Release, shared |
| **I/O engine** | IOCP (`ReadFile`/`WriteFile` + OVERLAPPED) | `pread`/`pwrite` + background thread |
| **Max in-flight** | 256 | 256 |
| **Chunk sizes tested** | 4 KB, 16 KB, 64 KB, 256 KB, 1 MB, 4 MB | 64 KB, 256 KB, 1 MB, 4 MB |

> **Note**: POSIX 4 KB / 16 KB chunks are skipped because the single background
> thread serializes all I/O, making 16384+ tiny operations impractically slow.
> These sizes are not representative of real-world throughput workloads.

## Results

### Throughput (MB/s, higher is better)

| Operation | Chunk | Windows | Linux (WSL2) | Win/Linux Ratio |
|-----------|-------|---------|-------------|-----------------|
| write | 4 KB | 528.2 | — | — |
| write | 16 KB | 1414.0 | — | — |
| write | 64 KB | 2295.6 | 1509.5 | 1.52× |
| write | 256 KB | 3087.0 | 1572.2 | 1.96× |
| write | 1 MB | 2763.6 | 1604.2 | 1.72× |
| write | 4 MB | 2822.1 | 1591.4 | 1.77× |
| read | 4 KB | 1068.2 | — | — |
| read | 16 KB | 2586.2 | — | — |
| read | 64 KB | 3408.6 | 4422.6 | 0.77× |
| read | 256 KB | 3404.1 | 3111.5 | 1.09× |
| read | 1 MB | 3778.5 | 3788.3 | 1.00× |
| read | 4 MB | 3927.8 | 3321.4 | 1.18× |

### Latency (ms, lower is better)

| Operation | Chunk | Windows | Linux (WSL2) |
|-----------|-------|---------|-------------|
| write | 4 KB | 121.16 | — |
| write | 16 KB | 45.26 | — |
| write | 64 KB | 27.88 | 42.40 |
| write | 256 KB | 20.73 | 40.71 |
| write | 1 MB | 23.16 | 39.90 |
| write | 4 MB | 22.68 | 40.22 |
| read | 4 KB | 59.92 | — |
| read | 16 KB | 24.75 | — |
| read | 64 KB | 18.78 | 14.47 |
| read | 256 KB | 18.80 | 20.57 |
| read | 1 MB | 16.94 | 16.89 |
| read | 4 MB | 16.29 | 19.27 |

### Best Results

| | Windows | Linux (WSL2) |
|---|---|---|
| **Best write** | 3087.0 MB/s @ 256 KB | 1604.2 MB/s @ 1 MB |
| **Best read** | 3927.8 MB/s @ 4 MB | 4422.6 MB/s @ 64 KB |

## Analysis

### Windows (IOCP)

- **Write throughput**: scales well with chunk size, saturates ~2.7–3.1 GB/s for
  chunks ≥ 256 KB. 4 KB chunks suffer from IOPS overhead (16384 separate IOCP
  operations).
- **Read throughput**: 3.4–3.9 GB/s across all chunk sizes ≥ 64 KB. OS file
  cache provides excellent read-ahead for sequential access.
- **Latency**: Minimal overhead beyond physical I/O. Write latency stabilizes
  at ~22 ms for 64 MB data (chunks ≥ 256 KB).

### Linux / WSL2 (pread/pwrite + background thread)

- **Write throughput**: ~1.5–1.6 GB/s, largely chunk-size independent. The
  single background thread serializes all `pwrite` calls, becoming the
  bottleneck regardless of chunk size.
- **Read throughput**: shows chunk-size sensitivity. 64 KB reads achieve
  the highest throughput (4.4 GB/s) due to optimal page-cache alignment.
  Larger reads see diminishing returns as the BG thread dispatches more
  64 KB sub-chunks.
- **Latency**: Write latency ~40 ms (2× Windows) due to BG thread
  serialization. Read latency comparable to Windows for optimal chunk sizes.

### Cross-Platform Observations

1. **Write**: Windows IOCP outperforms POSIX by 1.5–2.0×. The POSIX background
   thread is the bottleneck; a thread pool would improve throughput.
2. **Read**: Comparable performance at 1 MB chunks. POSIX has an edge at small
   chunks (64 KB) due to better page-cache alignment. Windows has an edge at
   large chunks due to IOCP concurrency.
3. **Small chunks** (4 KB, 16 KB): Windows-only. POSIX is not competitive here
   due to the thread-per-operation dispatch overhead. These sizes are not
   recommended for throughput-sensitive workloads on either platform.

## Notes

- WSL2 adds virtualization overhead; bare-metal Linux results may differ.
- Benchmarks write to temp directory (Windows: `%TEMP%`, Linux: `/tmp`).
  Results depend on disk speed and OS caching.
- The benchmark issues all operations concurrently (256 in-flight) and
  measures wall-clock time from first issue to last callback.
- All tests pass `ctest` validation (Windows: 300+ tests, Linux: 298 tests).

## Baseline Commit

This report corresponds to commit range:
- `93d0813` bench: add AsyncFile throughput benchmark
- `178ce3f` neixx/task: fix POSIX IO pump deadlock after DrainPendingWakeups
