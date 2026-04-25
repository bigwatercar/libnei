# Log Compare Benchmark

Generated: 2026-04-25 18:31:07
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Section | Avg us/log | Avg logs/sec |
|---|---|---|---:|---:|
| Best | [spdlog] file sync simple (async logger + flush request each log) | File (per-call flush request over async pipeline) | 0.1651 | 6067713 |
| Worst | [NEI] file strict multi (sync flush each log) | File (strict sync flush semantics) | 11.7137 | 90156 |

## Memory vs File Ratios

| Library | Scenario | Memory avg us/log | File avg us/log | File/Memory x |
|---|---|---:|---:|---:|
| NEI | simple | 0.3525 | 0.4410 | 1.25 |
| spdlog | simple | 0.4784 | 0.4267 | 0.89 |
| NEI | multi | 0.6069 | 0.4989 | 0.82 |
| spdlog | multi | 0.5277 | 0.3784 | 0.72 |
| NEI | literal | 0.2830 | 0.3663 | 1.29 |
| NEI | vlog_literal | 0.2922 | 0.3446 | 1.18 |

## Memory (async, minimal sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI]  simple %s | 3 | 1000000 | 352485.00 | 0.3525 | 0.0065 | 2837970 | - |
| [spdlog] simple {} | 3 | 1000000 | 478416.00 | 0.4784 | 0.0102 | 2091193 | - |
| [NEI]  multi printf | 3 | 1000000 | 606933.67 | 0.6069 | 0.0249 | 1650360 | - |
| [spdlog] multi fmt | 3 | 1000000 | 527682.00 | 0.5277 | 0.0679 | 1929903 | - |
| [NEI]  multi printf (fmt_plan cache miss) | 3 | 1000000 | 645636.33 | 0.6456 | 0.0051 | 1548957 | - |
| [NEI]  llog_literal (opaque body) | 3 | 1000000 | 283021.00 | 0.2830 | 0.0102 | 3537937 | - |
| [spdlog] literal only | 3 | 1000000 | 250906.00 | 0.2509 | 0.0210 | 4012173 | - |
| [NEI]  vlog_literal (opaque body) | 3 | 1000000 | 292183.33 | 0.2922 | 0.0094 | 3426117 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI]  simple %s | 2284597 | 1.00 | 1.33 | 1026.00 |
| [spdlog] simple {} | - | - | - | - |
| [NEI]  multi printf | 6956492 | 1.00 | 1.67 | 1026.00 |
| [spdlog] multi fmt | - | - | - | - |
| [NEI]  multi printf (fmt_plan cache miss) | 7464438 | 1.00 | 3.00 | 1026.00 |
| [NEI]  llog_literal (opaque body) | 1790819 | 1.00 | 3.33 | 1026.00 |
| [spdlog] literal only | - | - | - | - |
| [NEI]  vlog_literal (opaque body) | 1648825 | 1.00 | 2.00 | 1026.00 |

## File (async file sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file simple %s | 3 | 100000 | 44095.00 | 0.4410 | 0.0124 | 2269620 | 8200000 |
| [spdlog] file simple {} | 3 | 100000 | 42674.67 | 0.4267 | 0.0604 | 2387003 | 6700000 |
| [NEI] file multi | 3 | 100000 | 49892.00 | 0.4989 | 0.0552 | 2027480 | 9700000 |
| [spdlog] file multi | 3 | 100000 | 37841.00 | 0.3784 | 0.0047 | 2643047 | 8200000 |
| [NEI] file llog_literal | 3 | 100000 | 36629.33 | 0.3663 | 0.0267 | 2744203 | 8200000 |
| [NEI] file vlog_literal | 3 | 100000 | 34455.67 | 0.3446 | 0.0036 | 2902590 | 8200000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file simple %s | 326113 | 1.00 | 1.67 | 1026.00 |
| [spdlog] file simple {} | - | - | - | - |
| [NEI] file multi | 388731 | 1.00 | 1.00 | 1026.00 |
| [spdlog] file multi | - | - | - | - |
| [NEI] file llog_literal | 247776 | 1.00 | 1.00 | 1026.00 |
| [NEI] file vlog_literal | 238527 | 1.00 | 1.00 | 1025.67 |

## File (per-call flush request over async pipeline)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 3 | 10000 | 9303.67 | 0.9304 | 0.0111 | 1074997 | 820000 |
| [spdlog] file sync simple (async logger + flush request each log) | 3 | 10000 | 1651.33 | 0.1651 | 0.0074 | 6067713 | 670000 |
| [NEI] file sync multi (flush request each log) | 3 | 10000 | 9529.67 | 0.9530 | 0.0156 | 1049640 | 970000 |
| [spdlog] file sync multi (async logger + flush request each log) | 3 | 10000 | 1964.67 | 0.1965 | 0.0114 | 5108000 | 820000 |
| [NEI] file sync llog_literal (flush request each log) | 3 | 10000 | 18817.33 | 1.8817 | 1.3520 | 814202 | 820000 |
| [NEI] file sync vlog_literal (flush request each log) | 3 | 10000 | 11014.67 | 1.1015 | 0.3518 | 991702 | 820000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 0 | 0.00 | 2.00 | 1.00 |
| [spdlog] file sync simple (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync multi (flush request each log) | 0 | 0.00 | 1.00 | 1.00 |
| [spdlog] file sync multi (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync llog_literal (flush request each log) | 0 | 1.67 | 6.00 | 1.00 |
| [NEI] file sync vlog_literal (flush request each log) | 0 | 0.00 | 3.67 | 1.00 |

## File (strict sync flush semantics)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 3 | 5000 | 57374.67 | 11.4749 | 1.2383 | 88259 | 410000 |
| [spdlog] file strict simple (sync flush each log) | 3 | 5000 | 46580.67 | 9.3161 | 1.2871 | 109322 | 370000 |
| [NEI] file strict multi (sync flush each log) | 3 | 5000 | 58568.67 | 11.7137 | 2.8667 | 90156 | 485000 |
| [spdlog] file strict multi (sync flush each log) | 3 | 5000 | 42331.00 | 8.4662 | 0.4119 | 118388 | 445000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 0 | 1.00 | 5.00 | 1.00 |
| [spdlog] file strict simple (sync flush each log) | - | - | - | - |
| [NEI] file strict multi (sync flush each log) | 0 | 1.67 | 8.00 | 1.00 |
| [spdlog] file strict multi (sync flush each log) | - | - | - | - |


