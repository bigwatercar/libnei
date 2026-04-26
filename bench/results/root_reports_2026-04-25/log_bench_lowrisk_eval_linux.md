# Log Benchmark

Generated: 2026-04-25 23:32:06
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Error | memory | 0.2125 | 4710320 |
| Worst | File Log with Formatting | file | 1.1250 | 890136 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Error | 0.2125 | 1.0580 | 4.98 |
| Info (literal) | 0.2246 | 0.9385 | 4.18 |
| Info | 0.2129 | 1.0379 | 4.87 |
| Verbose (literal) | 0.2307 | 1.1029 | 4.78 |
| Verbose | 0.2218 | 1.0534 | 4.75 |
| Warn | 0.2165 | 1.1197 | 5.17 |
| with Formatting | 0.3341 | 1.1250 | 3.37 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 212885.00 | 0.2129 | 44.33 | 212930.00 | 0.2129 | 0.0081 | 4703350 | - |
| Log Warn | 3 | 1000000 | 216496.00 | 0.2165 | 42.33 | 216539.00 | 0.2165 | 0.0125 | 4634220 | - |
| Log Error | 3 | 1000000 | 212425.00 | 0.2124 | 50.33 | 212475.00 | 0.2125 | 0.0062 | 4710320 | - |
| Log with Formatting | 3 | 1000000 | 334004.00 | 0.3340 | 65.67 | 334069.00 | 0.3341 | 0.0117 | 2997070 | - |
| Log Info (literal) | 3 | 1000000 | 224535.00 | 0.2245 | 46.00 | 224582.00 | 0.2246 | 0.0111 | 4463350 | - |
| Log Verbose | 3 | 1000000 | 221796.00 | 0.2218 | 46.67 | 221843.00 | 0.2218 | 0.0103 | 4517160 | - |
| Log Verbose (literal) | 3 | 1000000 | 230677.00 | 0.2307 | 48.00 | 230726.00 | 0.2307 | 0.0226 | 4374640 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 377651 | 0.00 | 12.00 | 258.00 |
| Log Warn | 385091 | 0.00 | 23.00 | 258.00 |
| Log Error | 366231 | 0.00 | 8.00 | 258.00 |
| Log with Formatting | 841688 | 0.00 | 18.00 | 257.67 |
| Log Info (literal) | 289077 | 0.00 | 24.00 | 258.00 |
| Log Verbose | 398560 | 0.00 | 29.67 | 257.67 |
| Log Verbose (literal) | 327430 | 0.00 | 33.00 | 258.00 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 103559.00 | 1.0356 | 233.67 | 103793.00 | 1.0379 | 0.0576 | 966528 | 9800000 |
| File Log Warn | 3 | 100000 | 111660.00 | 1.1166 | 309.67 | 111970.00 | 1.1197 | 0.0194 | 893359 | 9800000 |
| File Log Error | 3 | 100000 | 105414.00 | 1.0541 | 385.00 | 105800.00 | 1.0580 | 0.0813 | 950822 | 9800000 |
| File Log with Formatting | 3 | 100000 | 112127.00 | 1.1213 | 376.00 | 112504.00 | 1.1250 | 0.0428 | 890136 | 11300000 |
| File Log Verbose | 3 | 100000 | 105094.00 | 1.0509 | 246.67 | 105341.00 | 1.0534 | 0.0655 | 952839 | 10400000 |
| File Log Info (literal) | 3 | 100000 | 93596.30 | 0.9360 | 257.00 | 93854.00 | 0.9385 | 0.0738 | 1072346 | 10300000 |
| File Log Verbose (literal) | 3 | 100000 | 110040.00 | 1.1004 | 253.33 | 110294.00 | 1.1029 | 0.0576 | 909208 | 10100000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 490473 | 0.67 | 4.00 | 257.33 |
| File Log Warn | 495128 | 1.00 | 4.33 | 257.00 |
| File Log Error | 505963 | 1.00 | 2.67 | 257.67 |
| File Log with Formatting | 503766 | 1.00 | 4.33 | 257.00 |
| File Log Verbose | 500614 | 1.00 | 3.33 | 257.00 |
| File Log Info (literal) | 429051 | 1.00 | 3.33 | 257.00 |
| File Log Verbose (literal) | 479966 | 1.00 | 4.33 | 257.33 |

