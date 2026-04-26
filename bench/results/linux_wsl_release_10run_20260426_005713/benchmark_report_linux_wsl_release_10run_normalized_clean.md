# Benchmark Report (Linux WSL Release, normalized 10-run, clean)

- Result directory: c:\Personal\Projects\LibNei\libnei-src\bench\results\linux_wsl_release_10run_20260426_005713
- Generated at: 2026-04-26 01:01:28
- Data cleaning: non-positive latency/throughput samples are excluded from mean/std/CV.

## Scenario Summary

| Suite | Scenario | N_valid | Avg us/log mean | std | CV% | Throughput mean (logs/s) |
|---|---|---:|---:|---:|---:|---:|
| log_bench | File Log Error | 10 | 0.7952 | 0.1293 | 16.26 | 1280639 |
| log_bench | File Log Info | 10 | 0.7772 | 0.0548 | 7.05 | 1292465 |
| log_bench | File Log Info (literal) | 10 | 0.761 | 0.0262 | 3.44 | 1315473 |
| log_bench | File Log Verbose | 10 | 0.7468 | 0.0328 | 4.39 | 1341350 |
| log_bench | File Log Verbose (literal) | 10 | 0.7925 | 0.0631 | 7.96 | 1268725 |
| log_bench | File Log Warn | 10 | 0.7663 | 0.0484 | 6.31 | 1309384 |
| log_bench | File Log with Formatting | 10 | 0.8158 | 0.0532 | 6.52 | 1230621 |
| log_bench | Log Error | 10 | 0.1987 | 0.0135 | 6.79 | 5052333 |
| log_bench | Log Info | 10 | 0.2063 | 0.0098 | 4.77 | 4856314 |
| log_bench | Log Info (literal) | 10 | 0.2045 | 0.0121 | 5.93 | 4905271 |
| log_bench | Log Verbose | 10 | 0.2014 | 0.0126 | 6.26 | 4981579 |
| log_bench | Log Verbose (literal) | 10 | 0.2072 | 0.0139 | 6.69 | 4845620 |
| log_bench | Log Warn | 10 | 0.1984 | 0.0144 | 7.26 | 5065138 |
| log_bench | Log with Formatting | 10 | 0.2988 | 0.0219 | 7.33 | 3362448 |
| log_bench_compare | [NEI]  llog_literal (opaque body) | 10 | 0.1887 | 0.007 | 3.69 | 5306821 |
| log_bench_compare | [NEI]  multi printf | 10 | 0.2651 | 0.0067 | 2.53 | 3774516 |
| log_bench_compare | [NEI]  multi printf (fmt_plan cache miss) | 10 | 0.2951 | 0.0082 | 2.77 | 3391421 |
| log_bench_compare | [NEI]  simple %s | 10 | 0.1654 | 0.0058 | 3.52 | 6052222 |
| log_bench_compare | [NEI]  vlog_literal (opaque body) | 9 | 0.1716 | 0.0058 | 3.38 | 5832710 |
| log_bench_compare | [NEI] file llog_literal | 10 | 0.7278 | 0.0417 | 5.73 | 1377969 |
| log_bench_compare | [NEI] file multi | 10 | 0.7395 | 0.0344 | 4.65 | 1354997 |
| log_bench_compare | [NEI] file simple %s | 10 | 0.689 | 0.079 | 11.47 | 1468895 |
| log_bench_compare | [NEI] file strict multi (sync flush each log) | 10 | 92.7971 | 8.4472 | 9.1 | 10850 |
| log_bench_compare | [NEI] file strict simple (sync flush each log) | 10 | 88.3111 | 4.0978 | 4.64 | 11346 |
| log_bench_compare | [NEI] file sync llog_literal (flush request each log) | 10 | 1.3182 | 0.1857 | 14.09 | 771986 |
| log_bench_compare | [NEI] file sync multi (flush request each log) | 10 | 1.2426 | 0.1177 | 9.47 | 811463 |
| log_bench_compare | [NEI] file sync simple (flush request each log) | 10 | 1.1801 | 0.039 | 3.3 | 848244 |
| log_bench_compare | [NEI] file sync vlog_literal (flush request each log) | 10 | 1.2669 | 0.2711 | 21.4 | 815519 |
| log_bench_compare | [NEI] file vlog_literal | 10 | 0.7118 | 0.0337 | 4.73 | 1407607 |
| log_bench_compare | [spdlog] file multi | 10 | 1.4091 | 0.0601 | 4.27 | 710807 |
| log_bench_compare | [spdlog] file simple {} | 10 | 1.1884 | 0.064 | 5.39 | 843506 |
| log_bench_compare | [spdlog] file strict multi (sync flush each log) | 10 | 87.7898 | 2.4685 | 2.81 | 11399 |
| log_bench_compare | [spdlog] file strict simple (sync flush each log) | 10 | 88.5693 | 4.6511 | 5.25 | 11317 |
| log_bench_compare | [spdlog] file sync multi (async logger + flush request each log) | 10 | 0.1844 | 0.0405 | 21.97 | 5592040 |
| log_bench_compare | [spdlog] file sync simple (async logger + flush request each log) | 10 | 0.1468 | 0.0064 | 4.38 | 6825026 |
| log_bench_compare | [spdlog] literal only | 10 | 0.3452 | 0.011 | 3.2 | 2899473 |
| log_bench_compare | [spdlog] multi fmt | 10 | 0.4851 | 0.0128 | 2.64 | 2062547 |
| log_bench_compare | [spdlog] simple {} | 10 | 0.3594 | 0.0115 | 3.19 | 2784788 |

## NEI vs spdlog Ratios (clean)

| Pair | NEI us/log | spdlog us/log | spdlog/NEI | Winner |
|---|---:|---:|---:|---|
| [NEI]  simple %s vs [spdlog] simple {} | 0.1654 | 0.3594 | 2.1729 | NEI |
| [NEI]  multi printf vs [spdlog] multi fmt | 0.2651 | 0.4851 | 1.8301 | NEI |
| [NEI] file simple %s vs [spdlog] file simple {} | 0.689 | 1.1884 | 1.7249 | NEI |
| [NEI] file multi vs [spdlog] file multi | 0.7395 | 1.4091 | 1.9056 | NEI |
| [NEI] file sync simple (flush request each log) vs [spdlog] file sync simple (async logger + flush request each log) | 1.1801 | 0.1468 | 0.1244 | spdlog |
| [NEI] file sync multi (flush request each log) vs [spdlog] file sync multi (async logger + flush request each log) | 1.2426 | 0.1844 | 0.1484 | spdlog |
| [NEI] file strict simple (sync flush each log) vs [spdlog] file strict simple (sync flush each log) | 88.3111 | 88.5693 | 1.0029 | NEI |
| [NEI] file strict multi (sync flush each log) vs [spdlog] file strict multi (sync flush each log) | 92.7971 | 87.7898 | 0.946 | spdlog |
