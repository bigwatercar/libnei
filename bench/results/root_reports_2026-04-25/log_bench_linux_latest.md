# Log Benchmark

Generated: 2026-04-25 18:25:24
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Error | memory | 0.2257 | 4434210 |
| Worst | File Log with Formatting | file | 1.1935 | 896116 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Error | 0.2257 | 1.1401 | 5.05 |
| Info (literal) | 0.2604 | 1.0433 | 4.01 |
| Info | 0.2264 | 0.9621 | 4.25 |
| Verbose (literal) | 0.2413 | 1.0586 | 4.39 |
| Verbose | 0.2323 | 1.1025 | 4.75 |
| Warn | 0.2358 | 1.0208 | 4.33 |
| with Formatting | 0.3337 | 1.1935 | 3.58 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 226380.00 | 0.2264 | 47.00 | 226427.00 | 0.2264 | 0.0073 | 4421050 | - |
| Log Warn | 3 | 1000000 | 235757.00 | 0.2358 | 44.00 | 235801.00 | 0.2358 | 0.0161 | 4261840 | - |
| Log Error | 3 | 1000000 | 225681.00 | 0.2257 | 40.33 | 225722.00 | 0.2257 | 0.0068 | 4434210 | - |
| Log with Formatting | 3 | 1000000 | 333650.00 | 0.3337 | 81.67 | 333732.00 | 0.3337 | 0.0035 | 2996740 | - |
| Log Info (literal) | 3 | 1000000 | 260320.00 | 0.2603 | 52.67 | 260374.00 | 0.2604 | 0.0252 | 3879580 | - |
| Log Verbose | 3 | 1000000 | 232181.00 | 0.2322 | 75.00 | 232257.00 | 0.2323 | 0.0070 | 4309520 | - |
| Log Verbose (literal) | 3 | 1000000 | 241275.00 | 0.2413 | 49.67 | 241326.00 | 0.2413 | 0.0199 | 4173730 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 378305 | 0.00 | 33.33 | 258.00 |
| Log Warn | 385671 | 0.00 | 31.67 | 258.00 |
| Log Error | 387064 | 0.00 | 35.00 | 257.67 |
| Log with Formatting | 898355 | 0.00 | 28.00 | 257.33 |
| Log Info (literal) | 407589 | 0.00 | 49.33 | 258.00 |
| Log Verbose | 430667 | 0.00 | 28.33 | 258.00 |
| Log Verbose (literal) | 385940 | 0.00 | 28.00 | 258.00 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 95841.00 | 0.9584 | 370.00 | 96211.70 | 0.9621 | 0.0695 | 1044980 | 9800000 |
| File Log Warn | 3 | 100000 | 101812.00 | 1.0181 | 265.33 | 102078.00 | 1.0208 | 0.1185 | 993815 | 9800000 |
| File Log Error | 3 | 100000 | 113780.00 | 1.1378 | 233.00 | 114014.00 | 1.1401 | 0.0478 | 878606 | 9800000 |
| File Log with Formatting | 3 | 100000 | 119095.00 | 1.1909 | 259.00 | 119354.00 | 1.1935 | 0.3300 | 896116 | 11300000 |
| File Log Verbose | 3 | 100000 | 109954.00 | 1.0995 | 294.00 | 110249.00 | 1.1025 | 0.1071 | 916233 | 10400000 |
| File Log Info (literal) | 3 | 100000 | 104044.00 | 1.0404 | 280.67 | 104326.00 | 1.0433 | 0.0800 | 964405 | 10300000 |
| File Log Verbose (literal) | 3 | 100000 | 105583.00 | 1.0558 | 275.33 | 105860.00 | 1.0586 | 0.0696 | 948763 | 10100000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 435363 | 1.00 | 2.33 | 257.33 |
| File Log Warn | 410400 | 1.00 | 1.67 | 257.00 |
| File Log Error | 437795 | 0.33 | 2.00 | 257.00 |
| File Log with Formatting | 429428 | 1.00 | 4.33 | 257.00 |
| File Log Verbose | 482705 | 1.00 | 2.67 | 257.33 |
| File Log Info (literal) | 475670 | 0.67 | 2.00 | 257.00 |
| File Log Verbose (literal) | 462186 | 1.00 | 2.67 | 257.33 |

