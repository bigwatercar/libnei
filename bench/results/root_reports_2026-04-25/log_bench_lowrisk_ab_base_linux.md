# Log Benchmark

Generated: 2026-04-25 23:35:17
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Verbose | memory | 0.1954 | 5119110 |
| Worst | File Log Verbose (literal) | file | 0.8544 | 1196850 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Error | 0.2020 | 0.7421 | 3.67 |
| Info (literal) | 0.2115 | 0.7664 | 3.62 |
| Info | 0.2051 | 0.7551 | 3.68 |
| Verbose (literal) | 0.2055 | 0.8544 | 4.16 |
| Verbose | 0.1954 | 0.7284 | 3.73 |
| Warn | 0.2055 | 0.7296 | 3.55 |
| with Formatting | 0.3111 | 0.7545 | 2.43 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 205034.00 | 0.2050 | 52.00 | 205087.00 | 0.2051 | 0.0046 | 4878450 | - |
| Log Warn | 3 | 1000000 | 205395.00 | 0.2054 | 58.00 | 205454.00 | 0.2055 | 0.0030 | 4868270 | - |
| Log Error | 3 | 1000000 | 201893.00 | 0.2019 | 57.33 | 201951.00 | 0.2020 | 0.0072 | 4958150 | - |
| Log with Formatting | 3 | 1000000 | 311011.00 | 0.3110 | 66.33 | 311078.00 | 0.3111 | 0.0115 | 3218900 | - |
| Log Info (literal) | 3 | 1000000 | 211491.00 | 0.2115 | 43.67 | 211535.00 | 0.2115 | 0.0070 | 4732470 | - |
| Log Verbose | 3 | 1000000 | 195390.00 | 0.1954 | 44.00 | 195435.00 | 0.1954 | 0.0041 | 5119110 | - |
| Log Verbose (literal) | 3 | 1000000 | 205404.00 | 0.2054 | 54.00 | 205458.00 | 0.2055 | 0.0082 | 4875220 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 365213 | 0.00 | 5.33 | 258.00 |
| Log Warn | 355680 | 0.00 | 4.67 | 257.67 |
| Log Error | 358094 | 0.00 | 3.67 | 258.00 |
| Log with Formatting | 867259 | 0.00 | 2.00 | 258.00 |
| Log Info (literal) | 365303 | 0.00 | 6.67 | 257.67 |
| Log Verbose | 375751 | 0.00 | 1.33 | 258.00 |
| Log Verbose (literal) | 348671 | 0.00 | 3.67 | 258.00 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 75310.00 | 0.7531 | 197.67 | 75508.30 | 0.7551 | 0.0515 | 1330290 | 9800000 |
| File Log Warn | 3 | 100000 | 72744.00 | 0.7274 | 219.67 | 72964.00 | 0.7296 | 0.0274 | 1372460 | 9800000 |
| File Log Error | 3 | 100000 | 74004.30 | 0.7400 | 207.33 | 74212.00 | 0.7421 | 0.0181 | 1348300 | 9800000 |
| File Log with Formatting | 3 | 100000 | 75256.00 | 0.7526 | 196.00 | 75452.30 | 0.7545 | 0.0143 | 1325820 | 11300000 |
| File Log Verbose | 3 | 100000 | 72669.00 | 0.7267 | 172.67 | 72842.00 | 0.7284 | 0.0116 | 1373180 | 10400000 |
| File Log Info (literal) | 3 | 100000 | 76450.30 | 0.7645 | 190.67 | 76641.70 | 0.7664 | 0.0139 | 1305210 | 10300000 |
| File Log Verbose (literal) | 3 | 100000 | 85133.00 | 0.8513 | 303.67 | 85437.30 | 0.8544 | 0.1337 | 1196850 | 10100000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 369815 | 1.00 | 2.33 | 257.33 |
| File Log Warn | 357094 | 0.33 | 1.00 | 257.00 |
| File Log Error | 363560 | 0.67 | 1.33 | 257.00 |
| File Log with Formatting | 367539 | 0.67 | 1.00 | 257.00 |
| File Log Verbose | 355166 | 0.33 | 1.00 | 257.00 |
| File Log Info (literal) | 370396 | 0.33 | 1.00 | 257.33 |
| File Log Verbose (literal) | 379655 | 0.67 | 2.67 | 257.33 |

