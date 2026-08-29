# libnei — Benchmark Baseline 2026-08-11

**Machine**: Intel Ultra 9 185H (22 cores), Windows 11 + WSL2 (GCC 13)
**Build**: Release shared (`windows-vs2022-shared`, `linux-gcc-release-shared`)
**Commit**: `558b3ed` — includes SHARED mode, flaky test fixes, IOThread singleton

---

## Task Scheduling

| Benchmark | Windows | WSL | Unit |
|-----------|:---:|:---:|------|
| `task_thread_bench` (1M tasks) | 417.0 | 337.2 | ns/task |
| `task_threadpool_bench` (1M tasks) | 339.3 | 353.1 | ns/task |

## File I/O

| Benchmark | Windows | WSL | Unit |
|-----------|:---:|:---:|------|
| `async_file_bench` best write | 3145.4 | 2129.4 | MB/s |
| `async_file_bench` best read | 3925.9 | 5185.1 | MB/s |

## Pipe I/O

| Chunk | Windows | WSL | Unit |
|-------|:---:|:---:|------|
| 4 KB | 420.7 | 3801.3 | MB/s |
| 16 KB | 619.0 | 5784.0 | MB/s |
| 64 KB | 2254.8 | 5949.6 | MB/s |
| 256 KB | 6052.0 | 5822.9 | MB/s |
| 1 MB | 3695.5 | 5583.1 | MB/s |

## TCP Loopback

| Buffer | Windows | WSL | Unit |
|--------|:---:|:---:|------|
| 128 KB | 3706.1 | — | MB/s |
| 256 KB | 3918.2 | 8672.5 | MB/s |
| 512 KB | 2936.5 | 9755.0 | MB/s |
| 1024 KB | 4144.0 | 10008.0 | MB/s |

## TCP Throughput

| Metric | Windows | WSL | Unit |
|--------|:---:|:---:|------|
| 10 MB @ 64 KB buf | 559.8 | 861.7 | MB/s |

## TLS Throughput

| Metric | Windows | WSL | Unit |
|--------|:---:|:---:|------|
| 10 MB @ 64 KB buf | 96.0 | 117.6 | MB/s |

---

## Notes

- **Pipe I/O**: WSL significantly outperforms Windows at small chunk sizes due to
  Linux kernel pipe buffer optimizations (splice, larger default buffers).
- **TCP**: WSL loopback throughput ~2x Windows, consistent with Linux kernel
  TCP stack advantages over Windows IOCP for localhost.
- **TLS**: Both platforms limited by mbedTLS software encryption; throughput
  scales with CPU frequency rather than I/O bandwidth.
- **Task scheduling**: WSL/GCC ~20% faster for single-thread PostTask;
  threadpool variant virtually tied (Windows IOCP overhead absent here).
- **IOThread singleton** (direction C): all benches use shared global IO thread.
  No measurable overhead vs per-bench dedicated IO threads (previous baseline
  within noise).
