# Log Compare Benchmark

Generated: 2026-04-25 23:49:05
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Section | Avg us/log | Avg logs/sec |
|---|---|---|---:|---:|
| Best | [spdlog] file sync multi (async logger + flush request each log) | File (per-call flush request over async pipeline) | 0.1959 | 5108420 |
| Worst | [NEI] file strict simple (sync flush each log) | File (strict sync flush semantics) | 11.3337 | 88687 |

## Memory vs File Ratios

| Library | Scenario | Memory avg us/log | File avg us/log | File/Memory x |
|---|---|---:|---:|---:|
| NEI | simple | 0.3588 | 0.4205 | 1.17 |
| spdlog | simple | 0.5247 | 0.3878 | 0.74 |
| NEI | multi | 0.5770 | 0.5078 | 0.88 |
| spdlog | multi | 0.4984 | 0.4317 | 0.87 |
| NEI | literal | 0.3195 | 0.4250 | 1.33 |
| NEI | vlog_literal | 0.3140 | 0.4136 | 1.32 |

## Memory (async, minimal sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI]  simple %s | 3 | 1000000 | 358804.00 | 0.3588 | 0.0136 | 2790940 | - |
| [spdlog] simple {} | 3 | 1000000 | 524741.67 | 0.5247 | 0.0416 | 1917733 | - |
| [NEI]  multi printf | 3 | 1000000 | 576983.67 | 0.5770 | 0.0132 | 1734043 | - |
| [spdlog] multi fmt | 3 | 1000000 | 498412.33 | 0.4984 | 0.0343 | 2016367 | - |
| [NEI]  multi printf (fmt_plan cache miss) | 3 | 1000000 | 614888.33 | 0.6149 | 0.0037 | 1626367 | - |
| [NEI]  llog_literal (opaque body) | 3 | 1000000 | 319505.33 | 0.3195 | 0.0097 | 3132673 | - |
| [spdlog] literal only | 3 | 1000000 | 259308.00 | 0.2593 | 0.0262 | 3897730 | - |
| [NEI]  vlog_literal (opaque body) | 3 | 1000000 | 313994.33 | 0.3140 | 0.0031 | 3185083 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI]  simple %s | 2417585 | 1.00 | 3.00 | 1026.00 |
| [spdlog] simple {} | - | - | - | - |
| [NEI]  multi printf | 6856384 | 1.00 | 2.33 | 1026.00 |
| [spdlog] multi fmt | - | - | - | - |
| [NEI]  multi printf (fmt_plan cache miss) | 7053442 | 1.00 | 1.67 | 1026.00 |
| [NEI]  llog_literal (opaque body) | 1999979 | 1.00 | 4.67 | 1026.00 |
| [spdlog] literal only | - | - | - | - |
| [NEI]  vlog_literal (opaque body) | 1930944 | 1.00 | 2.67 | 1026.00 |

## File (async file sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file simple %s | 3 | 100000 | 42051.67 | 0.4205 | 0.0295 | 2389310 | 8200000 |
| [spdlog] file simple {} | 3 | 100000 | 38779.67 | 0.3878 | 0.0202 | 2585457 | 6700000 |
| [NEI] file multi | 3 | 100000 | 50782.33 | 0.5078 | 0.0800 | 2014213 | 9700000 |
| [spdlog] file multi | 3 | 100000 | 43173.00 | 0.4317 | 0.0189 | 2320633 | 8200000 |
| [NEI] file llog_literal | 3 | 100000 | 42497.00 | 0.4250 | 0.0438 | 2377767 | 8200000 |
| [NEI] file vlog_literal | 3 | 100000 | 41361.67 | 0.4136 | 0.0236 | 2425457 | 8200000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file simple %s | 337973 | 1.00 | 1.00 | 1026.00 |
| [spdlog] file simple {} | - | - | - | - |
| [NEI] file multi | 383123 | 1.00 | 1.67 | 1026.00 |
| [spdlog] file multi | - | - | - | - |
| [NEI] file llog_literal | 332483 | 1.00 | 1.33 | 1026.00 |
| [NEI] file vlog_literal | 296283 | 1.00 | 1.67 | 1026.00 |

## File (per-call flush request over async pipeline)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 3 | 10000 | 11203.00 | 1.1203 | 0.3169 | 957468 | 820000 |
| [spdlog] file sync simple (async logger + flush request each log) | 3 | 10000 | 2433.33 | 0.2433 | 0.0448 | 4275513 | 670000 |
| [NEI] file sync multi (flush request each log) | 3 | 10000 | 9212.67 | 0.9213 | 0.0080 | 1085543 | 970000 |
| [spdlog] file sync multi (async logger + flush request each log) | 3 | 10000 | 1959.00 | 0.1959 | 0.0054 | 5108420 | 820000 |
| [NEI] file sync llog_literal (flush request each log) | 3 | 10000 | 8942.33 | 0.8942 | 0.1522 | 1148491 | 820000 |
| [NEI] file sync vlog_literal (flush request each log) | 3 | 10000 | 8905.00 | 0.8905 | 0.0960 | 1136521 | 820000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 0 | 0.00 | 4.67 | 1.00 |
| [spdlog] file sync simple (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync multi (flush request each log) | 0 | 0.00 | 1.67 | 1.00 |
| [spdlog] file sync multi (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync llog_literal (flush request each log) | 0 | 0.00 | 1.67 | 1.00 |
| [NEI] file sync vlog_literal (flush request each log) | 0 | 0.00 | 1.33 | 1.00 |

## File (strict sync flush semantics)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 3 | 5000 | 56668.67 | 11.3337 | 0.8197 | 88687 | 410000 |
| [spdlog] file strict simple (sync flush each log) | 3 | 5000 | 43015.00 | 8.6030 | 0.8471 | 117302 | 370000 |
| [NEI] file strict multi (sync flush each log) | 3 | 5000 | 44468.67 | 8.8937 | 0.1899 | 112491 | 485000 |
| [spdlog] file strict multi (sync flush each log) | 3 | 5000 | 50289.33 | 10.0579 | 1.0389 | 100468 | 445000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 0 | 1.33 | 8.33 | 1.00 |
| [spdlog] file strict simple (sync flush each log) | - | - | - | - |
| [NEI] file strict multi (sync flush each log) | 0 | 0.67 | 2.33 | 1.00 |
| [spdlog] file strict multi (sync flush each log) | - | - | - | - |


