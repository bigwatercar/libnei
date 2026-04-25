# Log Compare Benchmark

Generated: 2026-04-25 12:21:56
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Section | Avg us/log | Avg logs/sec |
|---|---|---|---:|---:|
| Best | [spdlog] file sync simple (async logger + flush request each log) | File (per-call flush request over async pipeline) | 0.1595 | 6347370 |
| Worst | [NEI] file strict simple (sync flush each log) | File (strict sync flush semantics) | 128.5580 | 7796 |

## Memory vs File Ratios

| Library | Scenario | Memory avg us/log | File avg us/log | File/Memory x |
|---|---|---:|---:|---:|
| NEI | literal | 0.1997 | 1.7616 | 8.82 |
| NEI | multi | 0.3157 | 1.7405 | 5.51 |
| NEI | simple | 0.1885 | 1.4929 | 7.92 |
| NEI | vlog_literal | 0.1793 | 1.6616 | 9.27 |
| spdlog | multi | 0.5231 | 1.7491 | 3.34 |
| spdlog | simple | 0.3570 | 1.5457 | 4.33 |

## Memory (async, minimal sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI]  simple %s | 3 | 1000000 | 188477.00 | 0.1885 | 0.0044 | 5308600 | - |
| [spdlog] simple {} | 3 | 1000000 | 356973.00 | 0.3570 | 0.0026 | 2801480 | - |
| [NEI]  multi printf | 3 | 1000000 | 315654.00 | 0.3157 | 0.0494 | 3240520 | - |
| [spdlog] multi fmt | 3 | 1000000 | 523110.00 | 0.5231 | 0.0356 | 1920110 | - |
| [NEI]  multi printf (fmt_plan cache miss) | 3 | 1000000 | 317249.00 | 0.3172 | 0.0150 | 3159010 | - |
| [NEI]  llog_literal (opaque body) | 3 | 1000000 | 199703.00 | 0.1997 | 0.0083 | 5015840 | - |
| [spdlog] literal only | 3 | 1000000 | 348081.00 | 0.3481 | 0.0122 | 2876350 | - |
| [NEI]  vlog_literal (opaque body) | 3 | 1000000 | 179303.00 | 0.1793 | 0.0012 | 5577370 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI]  simple %s | 320909 | 0.00 | 8.00 | 258.00 |
| [spdlog] simple {} | - | - | - | - |
| [NEI]  multi printf | 868614 | 0.00 | 14.33 | 258.00 |
| [spdlog] multi fmt | - | - | - | - |
| [NEI]  multi printf (fmt_plan cache miss) | 780281 | 0.00 | 7.00 | 258.00 |
| [NEI]  llog_literal (opaque body) | 344963 | 0.00 | 7.67 | 258.00 |
| [spdlog] literal only | - | - | - | - |
| [NEI]  vlog_literal (opaque body) | 263071 | 0.00 | 6.00 | 257.67 |

## File (async file sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file simple %s | 3 | 100000 | 149286.00 | 1.4929 | 0.2592 | 688363 | 8200000 |
| [spdlog] file simple {} | 3 | 100000 | 154568.00 | 1.5457 | 0.2249 | 659627 | 6600000 |
| [NEI] file multi | 3 | 100000 | 174046.00 | 1.7405 | 0.3992 | 601846 | 9700000 |
| [spdlog] file multi | 3 | 100000 | 174913.00 | 1.7491 | 0.2973 | 586996 | 8100000 |
| [NEI] file llog_literal | 3 | 100000 | 176159.00 | 1.7616 | 0.3607 | 589255 | 8200000 |
| [NEI] file vlog_literal | 3 | 100000 | 166161.00 | 1.6616 | 0.3761 | 630770 | 8200000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file simple %s | 680489 | 1.00 | 6.67 | 257.00 |
| [spdlog] file simple {} | - | - | - | - |
| [NEI] file multi | 727188 | 1.00 | 8.33 | 257.00 |
| [spdlog] file multi | - | - | - | - |
| [NEI] file llog_literal | 728971 | 1.00 | 6.33 | 257.33 |
| [NEI] file vlog_literal | 701143 | 1.00 | 9.00 | 257.00 |

## File (per-call flush request over async pipeline)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 3 | 10000 | 21352.70 | 2.1353 | 0.3674 | 481800 | 820000 |
| [spdlog] file sync simple (async logger + flush request each log) | 3 | 10000 | 1595.00 | 0.1595 | 0.0172 | 6347370 | 660000 |
| [NEI] file sync multi (flush request each log) | 3 | 10000 | 22894.00 | 2.2894 | 0.2084 | 440663 | 970000 |
| [spdlog] file sync multi (async logger + flush request each log) | 3 | 10000 | 1647.33 | 0.1647 | 0.0070 | 6080980 | 810000 |
| [NEI] file sync llog_literal (flush request each log) | 3 | 10000 | 20612.30 | 2.0612 | 0.2766 | 493361 | 820000 |
| [NEI] file sync vlog_literal (flush request each log) | 3 | 10000 | 23523.00 | 2.3523 | 0.3387 | 435157 | 820000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 0 | 0.67 | 1.00 | 1.00 |
| [spdlog] file sync simple (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync multi (flush request each log) | 0 | 1.00 | 1.33 | 1.00 |
| [spdlog] file sync multi (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync llog_literal (flush request each log) | 0 | 0.33 | 1.00 | 1.00 |
| [NEI] file sync vlog_literal (flush request each log) | 0 | 3.67 | 1.00 | 1.00 |

## File (strict sync flush semantics)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 3 | 5000 | 642790.00 | 128.5580 | 6.1981 | 7796 | 410000 |
| [spdlog] file strict simple (sync flush each log) | 3 | 5000 | 566012.00 | 113.2030 | 14.0220 | 8960 | 365000 |
| [NEI] file strict multi (sync flush each log) | 3 | 5000 | 610149.00 | 122.0300 | 12.3566 | 8277 | 485000 |
| [spdlog] file strict multi (sync flush each log) | 3 | 5000 | 542681.00 | 108.5360 | 6.9126 | 9253 | 440000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 0 | 2.00 | 6.33 | 1.00 |
| [spdlog] file strict simple (sync flush each log) | - | - | - | - |
| [NEI] file strict multi (sync flush each log) | 0 | 1.00 | 4.00 | 1.00 |
| [spdlog] file strict multi (sync flush each log) | - | - | - | - |

