# Log Benchmark

Generated: 2026-04-25 23:33:59
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Verbose | memory | 0.2141 | 4672260 |
| Worst | File Log Info | file | 1.1348 | 908045 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Error | 0.2191 | 1.1257 | 5.14 |
| Info (literal) | 0.2315 | 1.0471 | 4.52 |
| Info | 0.2303 | 1.1348 | 4.93 |
| Verbose (literal) | 0.2401 | 0.9956 | 4.15 |
| Verbose | 0.2141 | 1.0970 | 5.12 |
| Warn | 0.2202 | 1.0807 | 4.91 |
| with Formatting | 0.3419 | 1.0098 | 2.95 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 230290.00 | 0.2303 | 39.67 | 230331.00 | 0.2303 | 0.0179 | 4369030 | - |
| Log Warn | 3 | 1000000 | 220141.00 | 0.2201 | 55.33 | 220197.00 | 0.2202 | 0.0107 | 4552450 | - |
| Log Error | 3 | 1000000 | 219062.00 | 0.2191 | 45.33 | 219109.00 | 0.2191 | 0.0084 | 4570490 | - |
| Log with Formatting | 3 | 1000000 | 341813.00 | 0.3418 | 70.33 | 341884.00 | 0.3419 | 0.0041 | 2925390 | - |
| Log Info (literal) | 3 | 1000000 | 231457.00 | 0.2315 | 47.33 | 231505.00 | 0.2315 | 0.0151 | 4337290 | - |
| Log Verbose | 3 | 1000000 | 214027.00 | 0.2140 | 49.33 | 214076.00 | 0.2141 | 0.0032 | 4672260 | - |
| Log Verbose (literal) | 3 | 1000000 | 240048.00 | 0.2400 | 54.33 | 240103.00 | 0.2401 | 0.0115 | 4174730 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 418561 | 0.00 | 33.67 | 257.67 |
| Log Warn | 408914 | 0.00 | 12.00 | 258.00 |
| Log Error | 417766 | 0.00 | 15.00 | 257.67 |
| Log with Formatting | 916536 | 0.00 | 22.00 | 258.00 |
| Log Info (literal) | 394929 | 0.00 | 17.00 | 258.00 |
| Log Verbose | 444204 | 0.00 | 14.67 | 258.00 |
| Log Verbose (literal) | 417000 | 0.00 | 28.33 | 258.00 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 113162.00 | 1.1316 | 319.33 | 113482.00 | 1.1348 | 0.1977 | 908045 | 9800000 |
| File Log Warn | 3 | 100000 | 107778.00 | 1.0778 | 290.33 | 108068.00 | 1.0807 | 0.1395 | 939926 | 9800000 |
| File Log Error | 3 | 100000 | 112309.00 | 1.1231 | 260.33 | 112570.00 | 1.1257 | 0.1451 | 902618 | 9800000 |
| File Log with Formatting | 3 | 100000 | 100708.00 | 1.0071 | 269.33 | 100978.00 | 1.0098 | 0.1114 | 1002597 | 11300000 |
| File Log Verbose | 3 | 100000 | 109382.00 | 1.0938 | 311.67 | 109695.00 | 1.0970 | 0.1074 | 919864 | 10400000 |
| File Log Info (literal) | 3 | 100000 | 104473.00 | 1.0447 | 239.00 | 104712.00 | 1.0471 | 0.0539 | 957444 | 10300000 |
| File Log Verbose (literal) | 3 | 100000 | 99331.00 | 0.9933 | 224.67 | 99556.70 | 0.9956 | 0.0707 | 1009329 | 10100000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 477400 | 0.67 | 8.00 | 257.00 |
| File Log Warn | 443151 | 1.00 | 5.00 | 257.00 |
| File Log Error | 502832 | 1.00 | 3.33 | 257.33 |
| File Log with Formatting | 483048 | 0.67 | 1.67 | 257.00 |
| File Log Verbose | 495079 | 1.00 | 5.00 | 257.67 |
| File Log Info (literal) | 472597 | 1.00 | 5.33 | 257.00 |
| File Log Verbose (literal) | 464111 | 1.00 | 4.67 | 257.00 |

