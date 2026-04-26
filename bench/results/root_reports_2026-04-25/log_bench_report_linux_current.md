# Log Benchmark

Generated: 2026-04-25 23:48:22
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Error | memory | 0.2051 | 4879000 |
| Worst | File Log Warn | file | 1.1470 | 884393 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Error | 0.2051 | 1.0433 | 5.09 |
| Info (literal) | 0.2270 | 0.9983 | 4.40 |
| Info | 0.2101 | 1.1394 | 5.42 |
| Verbose (literal) | 0.2310 | 0.9622 | 4.17 |
| Verbose | 0.2197 | 1.0275 | 4.68 |
| Warn | 0.2179 | 1.1470 | 5.26 |
| with Formatting | 0.3369 | 1.1450 | 3.40 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 210058.00 | 0.2101 | 41.00 | 210099.00 | 0.2101 | 0.0130 | 4777550 | - |
| Log Warn | 3 | 1000000 | 217848.00 | 0.2178 | 43.67 | 217892.00 | 0.2179 | 0.0075 | 4594860 | - |
| Log Error | 3 | 1000000 | 205038.00 | 0.2050 | 44.67 | 205084.00 | 0.2051 | 0.0050 | 4879000 | - |
| Log with Formatting | 3 | 1000000 | 336775.00 | 0.3368 | 96.67 | 336872.00 | 0.3369 | 0.0221 | 2981190 | - |
| Log Info (literal) | 3 | 1000000 | 226969.00 | 0.2270 | 48.67 | 227018.00 | 0.2270 | 0.0113 | 4416220 | - |
| Log Verbose | 3 | 1000000 | 219614.00 | 0.2196 | 55.67 | 219670.00 | 0.2197 | 0.0062 | 4555990 | - |
| Log Verbose (literal) | 3 | 1000000 | 230906.00 | 0.2309 | 75.67 | 230982.00 | 0.2310 | 0.0204 | 4362770 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 371724 | 0.00 | 20.33 | 258.00 |
| Log Warn | 365171 | 0.00 | 25.00 | 258.00 |
| Log Error | 375120 | 0.00 | 10.67 | 258.00 |
| Log with Formatting | 893208 | 0.00 | 22.67 | 258.00 |
| Log Info (literal) | 368449 | 0.00 | 27.33 | 258.00 |
| Log Verbose | 421960 | 0.00 | 17.67 | 258.00 |
| Log Verbose (literal) | 389359 | 0.00 | 14.67 | 257.67 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 113611.00 | 1.1361 | 324.00 | 113936.00 | 1.1394 | 0.0563 | 879763 | 9800000 |
| File Log Warn | 3 | 100000 | 114429.00 | 1.1443 | 268.67 | 114698.00 | 1.1470 | 0.1423 | 884393 | 9800000 |
| File Log Error | 3 | 100000 | 103972.00 | 1.0397 | 362.67 | 104335.00 | 1.0433 | 0.0134 | 958609 | 9800000 |
| File Log with Formatting | 3 | 100000 | 114240.00 | 1.1424 | 261.67 | 114502.00 | 1.1450 | 0.0960 | 879292 | 11300000 |
| File Log Verbose | 3 | 100000 | 102512.00 | 1.0251 | 242.33 | 102755.00 | 1.0275 | 0.0297 | 974010 | 10400000 |
| File Log Info (literal) | 3 | 100000 | 99600.30 | 0.9960 | 224.33 | 99825.30 | 0.9983 | 0.0405 | 1003448 | 10300000 |
| File Log Verbose (literal) | 3 | 100000 | 96027.70 | 0.9603 | 192.67 | 96221.30 | 0.9622 | 0.1092 | 1052050 | 10100000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 491603 | 1.00 | 5.00 | 257.33 |
| File Log Warn | 491069 | 1.00 | 3.67 | 257.67 |
| File Log Error | 492540 | 1.00 | 2.67 | 257.33 |
| File Log with Formatting | 505653 | 1.00 | 3.67 | 257.33 |
| File Log Verbose | 464218 | 1.00 | 2.00 | 257.33 |
| File Log Info (literal) | 466484 | 0.67 | 1.33 | 257.00 |
| File Log Verbose (literal) | 439721 | 0.33 | 1.33 | 257.00 |

