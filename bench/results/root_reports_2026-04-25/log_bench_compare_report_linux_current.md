# Log Compare Benchmark

Generated: 2026-04-25 23:49:53
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Section | Avg us/log | Avg logs/sec |
|---|---|---|---:|---:|
| Best | [spdlog] file sync multi (async logger + flush request each log) | File (per-call flush request over async pipeline) | 0.1712 | 5869500 |
| Worst | [NEI] file strict simple (sync flush each log) | File (strict sync flush semantics) | 147.8760 | 6793 |

## Memory vs File Ratios

| Library | Scenario | Memory avg us/log | File avg us/log | File/Memory x |
|---|---|---:|---:|---:|
| NEI | literal | 0.2054 | 1.1959 | 5.82 |
| NEI | multi | 0.3106 | 1.1405 | 3.67 |
| NEI | simple | 0.1758 | 1.0831 | 6.16 |
| NEI | vlog_literal | 0.1959 | 0.9656 | 4.93 |
| spdlog | multi | 0.5198 | 2.4343 | 4.68 |
| spdlog | simple | 0.3807 | 2.0122 | 5.29 |

## Memory (async, minimal sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI]  simple %s | 3 | 1000000 | 175779.00 | 0.1758 | 0.0024 | 5690020 | - |
| [spdlog] simple {} | 3 | 1000000 | 380684.00 | 0.3807 | 0.0144 | 2630680 | - |
| [NEI]  multi printf | 3 | 1000000 | 310591.00 | 0.3106 | 0.0180 | 3230910 | - |
| [spdlog] multi fmt | 3 | 1000000 | 519788.00 | 0.5198 | 0.0085 | 1924380 | - |
| [NEI]  multi printf (fmt_plan cache miss) | 3 | 1000000 | 333671.00 | 0.3337 | 0.0026 | 2997150 | - |
| [NEI]  llog_literal (opaque body) | 3 | 1000000 | 205425.00 | 0.2054 | 0.0124 | 4886320 | - |
| [spdlog] literal only | 3 | 1000000 | 367735.00 | 0.3677 | 0.0028 | 2719510 | - |
| [NEI]  vlog_literal (opaque body) | 3 | 1000000 | 195887.00 | 0.1959 | 0.0052 | 5108660 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI]  simple %s | 278176 | 0.00 | 12.67 | 258.00 |
| [spdlog] simple {} | - | - | - | - |
| [NEI]  multi printf | 824151 | 0.00 | 26.67 | 258.00 |
| [spdlog] multi fmt | - | - | - | - |
| [NEI]  multi printf (fmt_plan cache miss) | 806636 | 0.00 | 10.33 | 258.00 |
| [NEI]  llog_literal (opaque body) | 334104 | 0.00 | 6.33 | 257.67 |
| [spdlog] literal only | - | - | - | - |
| [NEI]  vlog_literal (opaque body) | 276586 | 0.00 | 15.67 | 258.00 |

## File (async file sink)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file simple %s | 3 | 100000 | 108309.00 | 1.0831 | 0.0657 | 926672 | 8200000 |
| [spdlog] file simple {} | 3 | 100000 | 201221.00 | 2.0122 | 0.3685 | 512176 | 6600000 |
| [NEI] file multi | 3 | 100000 | 114049.00 | 1.1405 | 0.0466 | 878239 | 9700000 |
| [spdlog] file multi | 3 | 100000 | 243433.00 | 2.4343 | 0.2895 | 417128 | 8100000 |
| [NEI] file llog_literal | 3 | 100000 | 119594.00 | 1.1959 | 0.2043 | 858900 | 8200000 |
| [NEI] file vlog_literal | 3 | 100000 | 96562.30 | 0.9656 | 0.0526 | 1038620 | 8200000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file simple %s | 484951 | 1.00 | 1.33 | 257.33 |
| [spdlog] file simple {} | - | - | - | - |
| [NEI] file multi | 476609 | 1.00 | 1.33 | 257.67 |
| [spdlog] file multi | - | - | - | - |
| [NEI] file llog_literal | 463424 | 1.00 | 5.00 | 257.33 |
| [NEI] file vlog_literal | 441261 | 1.00 | 2.33 | 257.00 |

## File (per-call flush request over async pipeline)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 3 | 10000 | 17696.00 | 1.7696 | 0.1547 | 569728 | 820000 |
| [spdlog] file sync simple (async logger + flush request each log) | 3 | 10000 | 2731.33 | 0.2731 | 0.1954 | 5546690 | 660000 |
| [NEI] file sync multi (flush request each log) | 3 | 10000 | 16446.00 | 1.6446 | 0.0672 | 609055 | 970000 |
| [spdlog] file sync multi (async logger + flush request each log) | 3 | 10000 | 1711.67 | 0.1712 | 0.0116 | 5869500 | 810000 |
| [NEI] file sync llog_literal (flush request each log) | 3 | 10000 | 15233.70 | 1.5234 | 0.1774 | 664767 | 820000 |
| [NEI] file sync vlog_literal (flush request each log) | 3 | 10000 | 15320.70 | 1.5321 | 0.1626 | 659733 | 820000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file sync simple (flush request each log) | 0 | 0.33 | 1.00 | 1.00 |
| [spdlog] file sync simple (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync multi (flush request each log) | 0 | 0.00 | 1.00 | 1.00 |
| [spdlog] file sync multi (async logger + flush request each log) | - | - | - | - |
| [NEI] file sync llog_literal (flush request each log) | 0 | 0.00 | 1.00 | 1.00 |
| [NEI] file sync vlog_literal (flush request each log) | 0 | 0.00 | 1.00 | 1.00 |

## File (strict sync flush semantics)

| Benchmark | Runs | Iterations | Avg total us | Avg us/log | Stddev us/log | Avg logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 3 | 5000 | 739381.00 | 147.8760 | 10.0226 | 6793 | 410000 |
| [spdlog] file strict simple (sync flush each log) | 3 | 5000 | 647948.00 | 129.5900 | 5.8746 | 7732 | 365000 |
| [NEI] file strict multi (sync flush each log) | 3 | 5000 | 706795.00 | 141.3590 | 3.2129 | 7078 | 485000 |
| [spdlog] file strict multi (sync flush each log) | 3 | 5000 | 631776.00 | 126.3550 | 7.3063 | 7941 | 440000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| [NEI] file strict simple (sync flush each log) | 0 | 1.67 | 3.00 | 1.00 |
| [spdlog] file strict simple (sync flush each log) | - | - | - | - |
| [NEI] file strict multi (sync flush each log) | 0 | 1.33 | 3.67 | 1.00 |
| [spdlog] file strict multi (sync flush each log) | - | - | - | - |

