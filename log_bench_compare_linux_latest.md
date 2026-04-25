# Log Compare Benchmark

Generated: 2026-04-25 18:31:54
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Section | Avg us/log | Avg logs/sec |
|---|---|---|---:|---:|
| Best | [spdlog] file sync simple (async logger + flush request each log) | File (per-call flush request over async pipeline) | 0.1538 | 6558600 |
| Worst | [NEI] file strict simple (sync flush each log) | File (strict sync flush semantics) | 143.4480 | 6988 |

## Memory vs File Ratios

| Library | Scenario | Memory avg us/log | File avg us/log | File/Memory x |
|---|---|---:|---:|---:|
| NEI | literal | 0.2138 | 1.0299 | 4.82 |
| NEI | multi | 0.2996 | 1.0344 | 3.45 |
| NEI | simple | 0.1874 | 1.0313 | 5.50 |
| NEI | vlog_literal | 0.2076 | 0.9688 | 4.67 |
| spdlog | multi | 0.5120 | 2.0257 | 3.96 |
| spdlog | simple | 0.3606 | 1.6671 | 4.62 |

## Memory (async, minimal sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI]  simple %s | 3 | 1000000 | 187446.00 | 0.1874 | 0.0137 | 5363580 | - |
| [spdlog] simple {} | 3 | 1000000 | 360599.00 | 0.3606 | 0.0125 | 2776480 | - |
| [NEI]  multi printf | 3 | 1000000 | 299633.00 | 0.2996 | 0.0157 | 3346260 | - |
| [spdlog] multi fmt | 3 | 1000000 | 511957.00 | 0.5120 | 0.0114 | 1954260 | - |
| [NEI]  multi printf (fmt_plan cache miss) | 3 | 1000000 | 329629.00 | 0.3296 | 0.0128 | 3038340 | - |
| [NEI]  llog_literal (opaque body) | 3 | 1000000 | 213757.00 | 0.2138 | 0.0215 | 4723190 | - |
| [spdlog] literal only | 3 | 1000000 | 358221.00 | 0.3582 | 0.0191 | 2799300 | - |
| [NEI]  vlog_literal (opaque body) | 3 | 1000000 | 207552.00 | 0.2076 | 0.0118 | 4833050 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI]  simple %s | 304405 | 0.00 | 12.67 | 258.00 |
| [spdlog] simple {} | - | - | - | - |
| [NEI]  multi printf | 775190 | 0.00 | 9.33 | 258.00 |
| [spdlog] multi fmt | - | - | - | - |
| [NEI]  multi printf (fmt_plan cache miss) | 744582 | 0.00 | 13.33 | 257.67 |
| [NEI]  llog_literal (opaque body) | 333605 | 0.00 | 12.33 | 258.00 |
| [spdlog] literal only | - | - | - | - |
| [NEI]  vlog_literal (opaque body) | 288145 | 0.00 | 29.67 | 257.67 |

## File (async file sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file simple %s | 3 | 100000 | 103126.00 | 1.0313 | 0.2648 | 1033540 | 8200000 |
| [spdlog] file simple {} | 3 | 100000 | 166713.00 | 1.6671 | 0.1841 | 607799 | 6600000 |
| [NEI] file multi | 3 | 100000 | 103436.00 | 1.0344 | 0.0689 | 970966 | 9700000 |
| [spdlog] file multi | 3 | 100000 | 202572.00 | 2.0257 | 0.2175 | 499038 | 8100000 |
| [NEI] file llog_literal | 3 | 100000 | 102989.00 | 1.0299 | 0.1121 | 983508 | 8200000 |
| [NEI] file vlog_literal | 3 | 100000 | 96883.00 | 0.9688 | 0.0208 | 1032640 | 8200000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file simple %s | 430185 | 0.33 | 4.67 | 257.00 |
| [spdlog] file simple {} | - | - | - | - |
| [NEI] file multi | 446675 | 0.67 | 3.00 | 257.00 |
| [spdlog] file multi | - | - | - | - |
| [NEI] file llog_literal | 413355 | 1.00 | 3.67 | 257.33 |
| [NEI] file vlog_literal | 392782 | 0.67 | 1.67 | 257.00 |

## File (per-call flush request over async pipeline)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 3 | 10000 | 14651.70 | 1.4652 | 0.2071 | 695150 | 820000 |
| [spdlog] file sync simple (async logger + flush request each log) | 3 | 10000 | 1538.00 | 0.1538 | 0.0138 | 6558600 | 660000 |
| [NEI] file sync multi (flush request each log) | 3 | 10000 | 16205.70 | 1.6206 | 0.2052 | 626755 | 970000 |
| [spdlog] file sync multi (async logger + flush request each log) | 3 | 10000 | 1685.00 | 0.1685 | 0.0103 | 5958130 | 810000 |
| [NEI] file sync llog_literal (flush request each log) | 3 | 10000 | 12931.00 | 1.2931 | 0.0862 | 776665 | 820000 |
| [NEI] file sync vlog_literal (flush request each log) | 3 | 10000 | 12989.00 | 1.2989 | 0.1417 | 778623 | 820000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 0 | 0.00 | 1.00 | 1.00 |
| [spdlog] file sync simple (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync multi (flush request each log) | 0 | 0.00 | 1.00 | 1.00 |
| [spdlog] file sync multi (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync llog_literal (flush request each log) | 0 | 0.00 | 1.00 | 1.00 |
| [NEI] file sync vlog_literal (flush request each log) | 0 | 0.00 | 1.00 | 1.00 |

## File (strict sync flush semantics)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 3 | 5000 | 717241.00 | 143.4480 | 7.2499 | 6988 | 410000 |
| [spdlog] file strict simple (sync flush each log) | 3 | 5000 | 682884.00 | 136.5770 | 4.5942 | 7330 | 365000 |
| [NEI] file strict multi (sync flush each log) | 3 | 5000 | 684461.00 | 136.8920 | 3.0337 | 7309 | 485000 |
| [spdlog] file strict multi (sync flush each log) | 3 | 5000 | 706619.00 | 141.3240 | 5.6192 | 7087 | 440000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 0 | 2.33 | 5.33 | 1.00 |
| [spdlog] file strict simple (sync flush each log) | - | - | - | - |
| [NEI] file strict multi (sync flush each log) | 0 | 1.33 | 7.00 | 1.00 |
| [spdlog] file strict multi (sync flush each log) | - | - | - | - |

