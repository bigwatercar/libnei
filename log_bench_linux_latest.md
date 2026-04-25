# Log Benchmark

Generated: 2026-04-25 12:20:30
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Verbose | memory | 0.2234 | 4485680 |
| Worst | File Log Info (literal) | file | 2.2679 | 445721 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Error | 0.2309 | 2.2230 | 9.63 |
| Info (literal) | 0.2449 | 2.2679 | 9.26 |
| Info | 0.2259 | 1.8033 | 7.98 |
| Verbose (literal) | 0.2280 | 2.1539 | 9.45 |
| Verbose | 0.2234 | 2.0954 | 9.38 |
| Warn | 0.2318 | 2.0463 | 8.83 |
| with Formatting | 0.3289 | 2.1806 | 6.63 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 225850.00 | 0.2258 | 47.33 | 225898.00 | 0.2259 | 0.0082 | 4432720 | - |
| Log Warn | 3 | 1000000 | 231739.00 | 0.2317 | 58.67 | 231798.00 | 0.2318 | 0.0192 | 4342330 | - |
| Log Error | 3 | 1000000 | 230833.00 | 0.2308 | 40.67 | 230875.00 | 0.2309 | 0.0233 | 4374090 | - |
| Log with Formatting | 3 | 1000000 | 328810.00 | 0.3288 | 71.00 | 328881.00 | 0.3289 | 0.0115 | 3044340 | - |
| Log Info (literal) | 3 | 1000000 | 244803.00 | 0.2448 | 46.67 | 244850.00 | 0.2449 | 0.0253 | 4126370 | - |
| Log Verbose | 3 | 1000000 | 223374.00 | 0.2234 | 46.67 | 223422.00 | 0.2234 | 0.0106 | 4485680 | - |
| Log Verbose (literal) | 3 | 1000000 | 227964.00 | 0.2280 | 48.67 | 228013.00 | 0.2280 | 0.0142 | 4402630 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 370491 | 0.00 | 20.67 | 258.00 |
| Log Warn | 385337 | 0.00 | 45.67 | 258.00 |
| Log Error | 375762 | 0.00 | 21.33 | 257.67 |
| Log with Formatting | 869713 | 0.00 | 8.67 | 258.00 |
| Log Info (literal) | 401839 | 0.00 | 39.33 | 257.67 |
| Log Verbose | 438137 | 0.00 | 21.33 | 258.00 |
| Log Verbose (literal) | 405216 | 0.00 | 16.00 | 258.00 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 179915.00 | 1.7992 | 412.33 | 180328.00 | 1.8033 | 0.3224 | 571539 | 9800000 |
| File Log Warn | 3 | 100000 | 204166.00 | 2.0417 | 464.00 | 204631.00 | 2.0463 | 0.2578 | 496133 | 9800000 |
| File Log Error | 3 | 100000 | 221811.00 | 2.2181 | 487.00 | 222298.00 | 2.2230 | 0.1270 | 451380 | 9800000 |
| File Log with Formatting | 3 | 100000 | 217533.00 | 2.1753 | 526.67 | 218060.00 | 2.1806 | 0.1173 | 459877 | 11300000 |
| File Log Verbose | 3 | 100000 | 208928.00 | 2.0893 | 609.00 | 209543.00 | 2.0954 | 0.0286 | 477319 | 10400000 |
| File Log Info (literal) | 3 | 100000 | 226373.00 | 2.2637 | 420.33 | 226794.00 | 2.2679 | 0.2282 | 445721 | 10300000 |
| File Log Verbose (literal) | 3 | 100000 | 214529.00 | 2.1453 | 859.67 | 215389.00 | 2.1539 | 0.1504 | 466644 | 10100000 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 744837 | 1.00 | 11.33 | 257.00 |
| File Log Warn | 764697 | 1.00 | 18.33 | 257.00 |
| File Log Error | 756269 | 1.00 | 15.33 | 257.00 |
| File Log with Formatting | 782848 | 1.00 | 19.33 | 257.00 |
| File Log Verbose | 786281 | 1.00 | 12.00 | 257.00 |
| File Log Info (literal) | 788911 | 1.00 | 18.00 | 257.00 |
| File Log Verbose (literal) | 785790 | 1.00 | 18.33 | 257.00 |

