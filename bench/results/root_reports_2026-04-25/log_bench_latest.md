# Log Benchmark

Generated: 2026-04-25 23:47:54
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Info (literal) | memory | 0.3378 | 2965720 |
| Worst | Log with Formatting | memory | 0.5572 | 1795377 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Info | 0.3783 | 0.4106 | 1.09 |
| Warn | 0.3478 | 0.4197 | 1.21 |
| Error | 0.3418 | 0.4891 | 1.43 |
| with Formatting | 0.5572 | 0.4729 | 0.85 |
| Info (literal) | 0.3378 | 0.4053 | 1.20 |
| Verbose | 0.3647 | 0.4243 | 1.16 |
| Verbose (literal) | 0.3507 | 0.4187 | 1.19 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 377996.67 | 0.3780 | 253.67 | 378251.00 | 0.3783 | 0.0482 | 2684047 | - |
| Log Warn | 3 | 1000000 | 347617.33 | 0.3476 | 219.67 | 347837.67 | 0.3478 | 0.0108 | 2877747 | - |
| Log Error | 3 | 1000000 | 341572.00 | 0.3416 | 237.67 | 341810.00 | 0.3418 | 0.0143 | 2930600 | - |
| Log with Formatting | 3 | 1000000 | 556758.33 | 0.5568 | 463.00 | 557221.67 | 0.5572 | 0.0115 | 1795377 | - |
| Log Info (literal) | 3 | 1000000 | 337615.00 | 0.3376 | 186.33 | 337801.67 | 0.3378 | 0.0142 | 2965720 | - |
| Log Verbose | 3 | 1000000 | 364399.00 | 0.3644 | 300.33 | 364699.67 | 0.3647 | 0.0104 | 2744260 | - |
| Log Verbose (literal) | 3 | 1000000 | 350406.67 | 0.3504 | 262.67 | 350669.33 | 0.3507 | 0.0040 | 2852063 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 2259108 | 1.00 | 3.67 | 1026.00 |
| Log Warn | 2188074 | 1.00 | 1.67 | 1026.00 |
| Log Error | 2150082 | 1.00 | 2.33 | 1026.00 |
| Log with Formatting | 6662600 | 1.00 | 1.00 | 1026.00 |
| Log Info (literal) | 2107877 | 1.00 | 3.67 | 1026.00 |
| Log Verbose | 2610257 | 1.00 | 2.00 | 1026.00 |
| Log Verbose (literal) | 2376771 | 1.00 | 2.00 | 1026.00 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 40802.00 | 0.4080 | 253.33 | 41055.67 | 0.4106 | 0.0031 | 2435853 | 8833333 |
| File Log Warn | 3 | 100000 | 41635.33 | 0.4164 | 332.00 | 41968.33 | 0.4197 | 0.0353 | 2398803 | 8833333 |
| File Log Error | 3 | 100000 | 48543.33 | 0.4854 | 370.00 | 48914.33 | 0.4891 | 0.0240 | 2049263 | 8833333 |
| File Log with Formatting | 3 | 100000 | 46977.33 | 0.4698 | 313.00 | 47291.00 | 0.4729 | 0.0353 | 2125807 | 10333333 |
| File Log Verbose | 3 | 100000 | 42144.33 | 0.4214 | 287.67 | 42432.00 | 0.4243 | 0.0133 | 2359097 | 9433333 |
| File Log Info (literal) | 3 | 100000 | 40221.67 | 0.4022 | 306.33 | 40528.67 | 0.4053 | 0.0075 | 2468237 | 9333333 |
| File Log Verbose (literal) | 3 | 100000 | 41579.33 | 0.4158 | 294.00 | 41874.00 | 0.4187 | 0.0327 | 2402143 | 9133333 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 326127 | 1.00 | 1.67 | 1026.00 |
| File Log Warn | 339606 | 1.00 | 1.00 | 1026.00 |
| File Log Error | 358130 | 1.00 | 1.00 | 1026.00 |
| File Log with Formatting | 398972 | 1.00 | 1.00 | 1026.00 |
| File Log Verbose | 324279 | 1.00 | 1.00 | 1026.00 |
| File Log Info (literal) | 317830 | 1.00 | 1.00 | 1026.00 |
| File Log Verbose (literal) | 319570 | 1.00 | 1.00 | 1026.00 |


