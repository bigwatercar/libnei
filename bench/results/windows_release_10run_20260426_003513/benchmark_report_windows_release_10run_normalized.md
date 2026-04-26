# Benchmark Report (Windows Release, normalized 10-run)

- Result directory: c:\Personal\Projects\LibNei\libnei-src\bench\results\windows_release_10run_20260426_003513
- Generated at: 

## Scenario Summary

| Suite | Scenario | N | Avg us/log mean | std | CV% | Throughput mean (logs/s) | Diagnostics (mean) | File size mean (bytes) |
|---|---|---:|---:|---:|---:|---:|---|---:|
| log_bench | File Log Error | 10 | 0.3939 | 0.0095 | 2.42 | 2539907 | producer_spins=333839.3, flush_wait_loops=1, consumer_wakeups=1.2, ring_hwm=1026 | 8840000 |
| log_bench | File Log Info | 10 | 0.3914 | 0.0138 | 3.54 | 2557765 | producer_spins=336500.3, flush_wait_loops=1, consumer_wakeups=1.3, ring_hwm=1026 | 8840000 |
| log_bench | File Log Info (literal) | 10 | 0.3714 | 0.0179 | 4.83 | 2698434 | producer_spins=299051.7, flush_wait_loops=1, consumer_wakeups=1.1, ring_hwm=1025.9 | 9340000 |
| log_bench | File Log Verbose | 10 | 0.3853 | 0.007 | 1.82 | 2596394 | producer_spins=302034.2, flush_wait_loops=1, consumer_wakeups=1.1, ring_hwm=1025.9 | 9440000 |
| log_bench | File Log Verbose (literal) | 10 | 0.3576 | 0.0088 | 2.45 | 2797970 | producer_spins=290336.3, flush_wait_loops=1, consumer_wakeups=1.1, ring_hwm=1025.9 | 9140000 |
| log_bench | File Log Warn | 10 | 0.3879 | 0.0141 | 3.64 | 2580866 | producer_spins=330083, flush_wait_loops=1, consumer_wakeups=1, ring_hwm=1025.9 | 8840000 |
| log_bench | File Log with Formatting | 10 | 0.4303 | 0.0063 | 1.47 | 2324529 | producer_spins=404170.3, flush_wait_loops=1, consumer_wakeups=1, ring_hwm=1025.9 | 10340000 |
| log_bench | Log Error | 10 | 0.3061 | 0.008 | 2.6 | 3269149 | producer_spins=2167014.7, flush_wait_loops=1, consumer_wakeups=3.6, ring_hwm=1026 | - |
| log_bench | Log Info | 10 | 0.3039 | 0.0057 | 1.87 | 3292007 | producer_spins=2080693.4, flush_wait_loops=0.9, consumer_wakeups=4.1, ring_hwm=1026 | - |
| log_bench | Log Info (literal) | 10 | 0.2949 | 0.0082 | 2.79 | 3392879 | producer_spins=1931560.4, flush_wait_loops=1, consumer_wakeups=4.6, ring_hwm=1026 | - |
| log_bench | Log Verbose | 10 | 0.3375 | 0.0055 | 1.63 | 2963788 | producer_spins=2579048.9, flush_wait_loops=0.9, consumer_wakeups=4.9, ring_hwm=1026 | - |
| log_bench | Log Verbose (literal) | 10 | 0.3247 | 0.01 | 3.09 | 3081801 | producer_spins=2445876, flush_wait_loops=1, consumer_wakeups=4.4, ring_hwm=1026 | - |
| log_bench | Log Warn | 10 | 0.3001 | 0.0058 | 1.92 | 3333137 | producer_spins=2126138.3, flush_wait_loops=1, consumer_wakeups=6.7, ring_hwm=1026 | - |
| log_bench | Log with Formatting | 10 | 0.5153 | 0.0094 | 1.82 | 1941352 | producer_spins=6635803, flush_wait_loops=1, consumer_wakeups=2.2, ring_hwm=1026 | - |
| log_bench_compare | [NEI]  llog_literal (opaque body) | 10 | 0.2979 | 0.0116 | 3.9 | 3361491 | producer_spins=2137659.6, flush_wait_loops=0.9, consumer_wakeups=3.7, ring_hwm=1026 | - |
| log_bench_compare | [NEI]  multi printf | 10 | 0.5205 | 0.011 | 2.1 | 1921865 | producer_spins=6795468.4, flush_wait_loops=1, consumer_wakeups=1.7, ring_hwm=1026 | - |
| log_bench_compare | [NEI]  multi printf (fmt_plan cache miss) | 10 | 0.5715 | 0.0079 | 1.37 | 1750102 | producer_spins=7320121.9, flush_wait_loops=1, consumer_wakeups=1.6, ring_hwm=1026 | - |
| log_bench_compare | [NEI]  simple %s | 10 | 0.3198 | 0.0116 | 3.64 | 3130344 | producer_spins=2374334.8, flush_wait_loops=0.9, consumer_wakeups=2.6, ring_hwm=1026 | - |
| log_bench_compare | [NEI]  vlog_literal (opaque body) | 10 | 0.295 | 0.0177 | 5.99 | 3400724 | producer_spins=2032724.8, flush_wait_loops=1, consumer_wakeups=3.6, ring_hwm=1026 | - |
| log_bench_compare | [NEI] file llog_literal | 10 | 0.3726 | 0.0095 | 2.55 | 2685302 | producer_spins=307552, flush_wait_loops=1, consumer_wakeups=1.2, ring_hwm=1025.8 | - |
| log_bench_compare | [NEI] file multi | 10 | 0.4302 | 0.0174 | 4.04 | 2327517 | producer_spins=406409.3, flush_wait_loops=1, consumer_wakeups=1, ring_hwm=1026 | - |
| log_bench_compare | [NEI] file simple %s | 10 | 0.3874 | 0.0137 | 3.54 | 2583898 | producer_spins=337474.6, flush_wait_loops=1, consumer_wakeups=1.2, ring_hwm=1026 | - |
| log_bench_compare | [NEI] file strict multi (sync flush each log) | 10 | 11.1295 | 1.4851 | 13.34 | 91443 | producer_spins=0, flush_wait_loops=1.8, consumer_wakeups=3.5, ring_hwm=1 | - |
| log_bench_compare | [NEI] file strict simple (sync flush each log) | 10 | 9.8793 | 1.6711 | 16.91 | 103660 | producer_spins=0, flush_wait_loops=1.4, consumer_wakeups=6.3, ring_hwm=1 | - |
| log_bench_compare | [NEI] file sync llog_literal (flush request each log) | 10 | 0.8015 | 0.0651 | 8.12 | 1254818 | producer_spins=0, flush_wait_loops=0.2, consumer_wakeups=1.9, ring_hwm=1 | - |
| log_bench_compare | [NEI] file sync multi (flush request each log) | 10 | 0.892 | 0.086 | 9.64 | 1130794 | producer_spins=0, flush_wait_loops=0.1, consumer_wakeups=1.9, ring_hwm=1 | - |
| log_bench_compare | [NEI] file sync simple (flush request each log) | 10 | 0.8069 | 0.0728 | 9.02 | 1248430 | producer_spins=0, flush_wait_loops=0, consumer_wakeups=1.5, ring_hwm=1 | - |
| log_bench_compare | [NEI] file sync vlog_literal (flush request each log) | 10 | 0.7808 | 0.0598 | 7.66 | 1287570 | producer_spins=0, flush_wait_loops=0, consumer_wakeups=1.4, ring_hwm=1 | - |
| log_bench_compare | [NEI] file vlog_literal | 10 | 0.3608 | 0.0069 | 1.9 | 2772829 | producer_spins=288787.8, flush_wait_loops=1, consumer_wakeups=1, ring_hwm=1025.9 | - |
| log_bench_compare | [spdlog] file multi | 10 | 0.3681 | 0.0263 | 7.16 | 2728481 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |
| log_bench_compare | [spdlog] file simple {} | 10 | 0.3612 | 0.0293 | 8.1 | 2784800 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |
| log_bench_compare | [spdlog] file strict multi (sync flush each log) | 10 | 7.5603 | 0.123 | 1.63 | 132300 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |
| log_bench_compare | [spdlog] file strict simple (sync flush each log) | 10 | 7.5625 | 0.3618 | 4.78 | 132489 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |
| log_bench_compare | [spdlog] file sync multi (async logger + flush request each log) | 10 | 0.1966 | 0.0204 | 10.36 | 5131394 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |
| log_bench_compare | [spdlog] file sync simple (async logger + flush request each log) | 10 | 0.1897 | 0.0395 | 20.85 | 5448655 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |
| log_bench_compare | [spdlog] literal only | 10 | 0.2243 | 0.0254 | 11.3 | 4507184 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |
| log_bench_compare | [spdlog] multi fmt | 10 | 0.4594 | 0.0371 | 8.07 | 2189362 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |
| log_bench_compare | [spdlog] simple {} | 10 | 0.5095 | 0.0917 | 18 | 2018782 | producer_spins=-, flush_wait_loops=-, consumer_wakeups=-, ring_hwm=- | - |

## NEI vs spdlog Ratios (from log_bench_compare, lower us/log is better)

| Pair | NEI us/log | spdlog us/log | spdlog/NEI | Winner |
|---|---:|---:|---:|---|
| [NEI]  simple %s vs [spdlog] simple {} | 0.3198 | 0.5095 | 1.5931 | NEI |
| [NEI]  multi printf vs [spdlog] multi fmt | 0.5205 | 0.4594 | 0.8826 | spdlog |
| [NEI] file simple %s vs [spdlog] file simple {} | 0.3874 | 0.3612 | 0.9322 | spdlog |
| [NEI] file multi vs [spdlog] file multi | 0.4302 | 0.3681 | 0.8556 | spdlog |
| [NEI] file sync simple (flush request each log) vs [spdlog] file sync simple (async logger + flush request each log) | 0.8069 | 0.1897 | 0.2351 | spdlog |
