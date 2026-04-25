# Log Compare Benchmark

Generated: 2026-04-25 12:19:01
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Section | Avg us/log | Avg logs/sec |
|---|---|---|---:|---:|
| Best | [spdlog] file sync simple (async logger + flush request each log) | File (per-call flush request over async pipeline) | 0.1631 | 6151427 |
| Worst | [NEI] file strict multi (sync flush each log) | File (strict sync flush semantics) | 10.5851 | 96800 |

## Memory vs File Ratios

| Library | Scenario | Memory avg us/log | File avg us/log | File/Memory x |
|---|---|---:|---:|---:|
| NEI | simple | 0.3626 | 0.4430 | 1.22 |
| spdlog | simple | 0.4847 | 0.3645 | 0.75 |
| NEI | multi | 0.6184 | 0.5213 | 0.84 |
| spdlog | multi | 0.5483 | 0.4123 | 0.75 |
| NEI | literal | 0.2902 | 0.3638 | 1.25 |
| NEI | vlog_literal | 0.2882 | 0.3552 | 1.23 |

## Memory (async, minimal sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI]  simple %s | 3 | 1000000 | 362575.67 | 0.3626 | 0.0162 | 2763520 | - |
| [spdlog] simple {} | 3 | 1000000 | 484739.33 | 0.4847 | 0.0269 | 2069303 | - |
| [NEI]  multi printf | 3 | 1000000 | 618439.33 | 0.6184 | 0.0266 | 1619883 | - |
| [spdlog] multi fmt | 3 | 1000000 | 548262.00 | 0.5483 | 0.0385 | 1832590 | - |
| [NEI]  multi printf (fmt_plan cache miss) | 3 | 1000000 | 641482.67 | 0.6415 | 0.0156 | 1559790 | - |
| [NEI]  llog_literal (opaque body) | 3 | 1000000 | 290194.67 | 0.2902 | 0.0161 | 3456237 | - |
| [spdlog] literal only | 3 | 1000000 | 272335.00 | 0.2723 | 0.0302 | 3721273 | - |
| [NEI]  vlog_literal (opaque body) | 3 | 1000000 | 288162.00 | 0.2882 | 0.0196 | 3485767 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI]  simple %s | 2403486 | 1.00 | 1.67 | 1026.00 |
| [spdlog] simple {} | - | - | - | - |
| [NEI]  multi printf | 7455607 | 1.00 | 1.33 | 1026.00 |
| [spdlog] multi fmt | - | - | - | - |
| [NEI]  multi printf (fmt_plan cache miss) | 7256657 | 1.00 | 5.33 | 1026.00 |
| [NEI]  llog_literal (opaque body) | 1712344 | 1.00 | 3.00 | 1026.00 |
| [spdlog] literal only | - | - | - | - |
| [NEI]  vlog_literal (opaque body) | 1567476 | 1.00 | 3.67 | 1026.00 |

## File (async file sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file simple %s | 3 | 100000 | 44299.33 | 0.4430 | 0.0064 | 2257843 | 8200000 |
| [spdlog] file simple {} | 3 | 100000 | 36445.33 | 0.3645 | 0.0169 | 2749737 | 6700000 |
| [NEI] file multi | 3 | 100000 | 52131.00 | 0.5213 | 0.0199 | 1921107 | 9700000 |
| [spdlog] file multi | 3 | 100000 | 41226.33 | 0.4123 | 0.0156 | 2429063 | 8200000 |
| [NEI] file llog_literal | 3 | 100000 | 36376.33 | 0.3638 | 0.0192 | 2756987 | 8200000 |
| [NEI] file vlog_literal | 3 | 100000 | 35517.33 | 0.3552 | 0.0047 | 2816027 | 8200000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file simple %s | 319792 | 1.00 | 1.33 | 1026.00 |
| [spdlog] file simple {} | - | - | - | - |
| [NEI] file multi | 413585 | 1.00 | 1.33 | 1026.00 |
| [spdlog] file multi | - | - | - | - |
| [NEI] file llog_literal | 285091 | 1.00 | 1.00 | 1025.67 |
| [NEI] file vlog_literal | 250006 | 1.00 | 1.33 | 1026.00 |

## File (per-call flush request over async pipeline)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 3 | 10000 | 12900.00 | 1.2900 | 0.4614 | 863354 | 820000 |
| [spdlog] file sync simple (async logger + flush request each log) | 3 | 10000 | 1631.33 | 0.1631 | 0.0095 | 6151427 | 670000 |
| [NEI] file sync multi (flush request each log) | 3 | 10000 | 11059.33 | 1.1059 | 0.1275 | 915983 | 970000 |
| [spdlog] file sync multi (async logger + flush request each log) | 3 | 10000 | 2005.33 | 0.2005 | 0.0117 | 5003017 | 820000 |
| [NEI] file sync llog_literal (flush request each log) | 3 | 10000 | 13511.33 | 1.3511 | 0.3025 | 785510 | 820000 |
| [NEI] file sync vlog_literal (flush request each log) | 3 | 10000 | 9805.00 | 0.9805 | 0.0975 | 1029496 | 820000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 0 | 0.00 | 7.67 | 1.00 |
| [spdlog] file sync simple (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync multi (flush request each log) | 0 | 0.33 | 1.67 | 1.00 |
| [spdlog] file sync multi (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync llog_literal (flush request each log) | 0 | 0.00 | 7.67 | 1.00 |
| [NEI] file sync vlog_literal (flush request each log) | 0 | 0.00 | 3.00 | 1.00 |

## File (strict sync flush semantics)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 3 | 5000 | 50218.00 | 10.0436 | 0.4994 | 99805 | 410000 |
| [spdlog] file strict simple (sync flush each log) | 3 | 5000 | 43837.00 | 8.7674 | 0.6275 | 114647 | 370000 |
| [NEI] file strict multi (sync flush each log) | 3 | 5000 | 52925.33 | 10.5851 | 1.7337 | 96800 | 485000 |
| [spdlog] file strict multi (sync flush each log) | 3 | 5000 | 44231.00 | 8.8462 | 0.9142 | 114208 | 445000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 0 | 1.00 | 3.33 | 1.00 |
| [spdlog] file strict simple (sync flush each log) | - | - | - | - |
| [NEI] file strict multi (sync flush each log) | 0 | 1.00 | 4.00 | 1.00 |
| [spdlog] file strict multi (sync flush each log) | - | - | - | - |


