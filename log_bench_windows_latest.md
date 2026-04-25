# Log Benchmark

Generated: 2026-04-25 12:18:31
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Info (literal) | memory | 0.2933 | 3416113 |
| Worst | Log with Formatting | memory | 0.5721 | 1748843 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Info | 0.3506 | 0.4788 | 1.37 |
| Warn | 0.3486 | 0.4643 | 1.33 |
| Error | 0.3385 | 0.4311 | 1.27 |
| with Formatting | 0.5721 | 0.5029 | 0.88 |
| Info (literal) | 0.2933 | 0.3796 | 1.29 |
| Verbose | 0.3838 | 0.4684 | 1.22 |
| Verbose (literal) | 0.3040 | 0.4386 | 1.44 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 350393.33 | 0.3504 | 204.67 | 350598.00 | 0.3506 | 0.0072 | 2853497 | - |
| Log Warn | 3 | 1000000 | 348371.00 | 0.3484 | 235.33 | 348606.33 | 0.3486 | 0.0112 | 2871540 | - |
| Log Error | 3 | 1000000 | 338321.00 | 0.3383 | 208.33 | 338530.00 | 0.3385 | 0.0128 | 2958167 | - |
| Log with Formatting | 3 | 1000000 | 571648.67 | 0.5716 | 414.00 | 572062.67 | 0.5721 | 0.0121 | 1748843 | - |
| Log Info (literal) | 3 | 1000000 | 293096.00 | 0.2931 | 187.00 | 293283.33 | 0.2933 | 0.0128 | 3416113 | - |
| Log Verbose | 3 | 1000000 | 383554.00 | 0.3836 | 218.00 | 383772.67 | 0.3838 | 0.0022 | 2605793 | - |
| Log Verbose (literal) | 3 | 1000000 | 303717.00 | 0.3037 | 250.00 | 303967.33 | 0.3040 | 0.0070 | 3291587 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 2077115 | 1.00 | 4.00 | 1026.00 |
| Log Warn | 2125505 | 1.00 | 2.33 | 1026.00 |
| Log Error | 2083655 | 1.00 | 6.33 | 1026.00 |
| Log with Formatting | 6680223 | 1.00 | 2.00 | 1026.00 |
| Log Info (literal) | 1571270 | 1.00 | 4.00 | 1026.00 |
| Log Verbose | 2683001 | 1.00 | 3.67 | 1026.00 |
| Log Verbose (literal) | 1841602 | 1.00 | 1.33 | 1026.00 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 47530.00 | 0.4753 | 347.67 | 47878.00 | 0.4788 | 0.0585 | 2117803 | 8866667 |
| File Log Warn | 3 | 100000 | 46106.67 | 0.4611 | 321.67 | 46429.00 | 0.4643 | 0.0659 | 2194023 | 8866667 |
| File Log Error | 3 | 100000 | 42799.00 | 0.4280 | 308.67 | 43108.67 | 0.4311 | 0.0240 | 2326667 | 8866667 |
| File Log with Formatting | 3 | 100000 | 49965.33 | 0.4997 | 322.67 | 50288.33 | 0.5029 | 0.0646 | 2019157 | 10366667 |
| File Log Verbose | 3 | 100000 | 46539.33 | 0.4654 | 295.00 | 46835.00 | 0.4684 | 0.0491 | 2158310 | 9466667 |
| File Log Info (literal) | 3 | 100000 | 37557.67 | 0.3756 | 402.33 | 37960.33 | 0.3796 | 0.0188 | 2640603 | 9366667 |
| File Log Verbose (literal) | 3 | 100000 | 43330.33 | 0.4333 | 527.67 | 43859.00 | 0.4386 | 0.0469 | 2304627 | 9166667 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 339384 | 1.00 | 1.67 | 1026.00 |
| File Log Warn | 339766 | 1.00 | 1.00 | 1026.00 |
| File Log Error | 339519 | 1.00 | 1.00 | 1026.00 |
| File Log with Formatting | 416585 | 1.00 | 1.00 | 1026.00 |
| File Log Verbose | 294556 | 1.00 | 1.00 | 1026.00 |
| File Log Info (literal) | 307085 | 1.00 | 1.00 | 1026.00 |
| File Log Verbose (literal) | 304480 | 1.00 | 1.33 | 1026.00 |


