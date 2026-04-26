# Log Benchmark

Generated: 2026-04-25 18:24:53
Runs: 3
Build: Release

## Highlights

| Type | Benchmark | Mode | Avg e2e us/log | Avg e2e logs/sec |
|---|---|---|---:|---:|
| Best | Log Info (literal) | memory | 0.2997 | 3348787 |
| Worst | Log with Formatting | memory | 0.5919 | 1690880 |

## Memory vs File Ratios

| Scenario | Memory avg e2e us/log | File avg e2e us/log | File/Memory x |
|---|---:|---:|---:|
| Info | 0.3525 | 0.4260 | 1.21 |
| Warn | 0.3513 | 0.4237 | 1.21 |
| Error | 0.3646 | 0.4214 | 1.16 |
| with Formatting | 0.5919 | 0.4658 | 0.79 |
| Info (literal) | 0.2997 | 0.3560 | 1.19 |
| Verbose | 0.3786 | 0.4269 | 1.13 |
| Verbose (literal) | 0.3212 | 0.3768 | 1.17 |

## In-memory sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Log Info | 3 | 1000000 | 352253.00 | 0.3523 | 268.67 | 352522.33 | 0.3525 | 0.0129 | 2840493 | - |
| Log Warn | 3 | 1000000 | 351084.67 | 0.3511 | 247.00 | 351332.00 | 0.3513 | 0.0105 | 2848933 | - |
| Log Error | 3 | 1000000 | 364305.00 | 0.3643 | 247.33 | 364553.00 | 0.3646 | 0.0118 | 2746007 | - |
| Log with Formatting | 3 | 1000000 | 591433.33 | 0.5914 | 462.33 | 591896.33 | 0.5919 | 0.0168 | 1690880 | - |
| Log Info (literal) | 3 | 1000000 | 299450.33 | 0.2995 | 203.00 | 299653.67 | 0.2997 | 0.0178 | 3348787 | - |
| Log Verbose | 3 | 1000000 | 378313.33 | 0.3783 | 264.00 | 378577.67 | 0.3786 | 0.0018 | 2641527 | - |
| Log Verbose (literal) | 3 | 1000000 | 320947.00 | 0.3209 | 206.00 | 321153.33 | 0.3212 | 0.0118 | 3117900 | - |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| Log Info | 2197459 | 1.00 | 2.67 | 1026.00 |
| Log Warn | 2166800 | 1.00 | 4.67 | 1026.00 |
| Log Error | 2276700 | 1.00 | 4.67 | 1026.00 |
| Log with Formatting | 6695921 | 1.00 | 2.00 | 1026.00 |
| Log Info (literal) | 1672722 | 1.00 | 1.67 | 1026.00 |
| Log Verbose | 2608173 | 1.00 | 1.67 | 1026.00 |
| Log Verbose (literal) | 1880604 | 1.00 | 3.33 | 1026.00 |

## File sink

| Benchmark | Runs | Iterations | Avg enqueue total us | Avg enqueue us/log | Avg flush total us | Avg e2e total us | Avg e2e us/log | Stddev e2e us/log | Avg e2e logs/sec | Avg file bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| File Log Info | 3 | 100000 | 42312.67 | 0.4231 | 287.67 | 42601.00 | 0.4260 | 0.0137 | 2349800 | 8866667 |
| File Log Warn | 3 | 100000 | 42004.00 | 0.4200 | 362.00 | 42366.33 | 0.4237 | 0.0185 | 2364780 | 8866667 |
| File Log Error | 3 | 100000 | 41857.67 | 0.4186 | 285.00 | 42142.67 | 0.4214 | 0.0085 | 2373857 | 8866667 |
| File Log with Formatting | 3 | 100000 | 46253.33 | 0.4625 | 325.00 | 46578.67 | 0.4658 | 0.0186 | 2150400 | 10366667 |
| File Log Verbose | 3 | 100000 | 42395.00 | 0.4240 | 295.67 | 42691.33 | 0.4269 | 0.0117 | 2344157 | 9466667 |
| File Log Info (literal) | 3 | 100000 | 35334.33 | 0.3533 | 261.67 | 35596.67 | 0.3560 | 0.0209 | 2818617 | 9366667 |
| File Log Verbose (literal) | 3 | 100000 | 37424.33 | 0.3742 | 251.33 | 37676.00 | 0.3768 | 0.0231 | 2664123 | 9166667 |

### Diagnostics

| Benchmark | Avg producer spins | Avg flush wait loops | Avg consumer wakeups | Avg ring HWM |
|---|---:|---:|---:|---:|
| File Log Info | 346873 | 1.00 | 1.00 | 1026.00 |
| File Log Warn | 298944 | 1.00 | 1.00 | 1025.67 |
| File Log Error | 329354 | 1.00 | 1.00 | 1026.00 |
| File Log with Formatting | 394425 | 1.00 | 1.00 | 1026.00 |
| File Log Verbose | 301998 | 1.00 | 1.33 | 1026.00 |
| File Log Info (literal) | 273192 | 1.00 | 1.00 | 1026.00 |
| File Log Verbose (literal) | 240515 | 1.00 | 1.33 | 1026.00 |


