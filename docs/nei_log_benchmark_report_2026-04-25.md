# Log 模块 Benchmark 详细报告（2026-04-25）

## 1. 结论摘要

- 本次已完成双平台 Release 模式的标准基准与 compare 基准采集。
- Windows：整体表现与基线接近，部分场景小幅波动；未出现“系统性退化”。
- Linux（WSL）：内存场景整体优于基线，但部分 file 场景出现显著回退，主要集中在 info/warn 与 compare 的 file/sync 类场景，疑似受文件系统与 flush 语义抖动影响。
- 对比结论：NEI 在 Linux compare 的 memory 与部分 file 场景仍有明显优势；在严格同步 flush 语义下，NEI 与 spdlog 均出现高延迟，结果对环境敏感。

## 2. 测试口径

- 构建模式：Release
- 轮次：3 runs
- 平台：
  - Windows（VS2022 Release）
  - Linux（WSL，build-linux）
- 报告来源：
  - 标准基准（当前）
    - Windows: log_bench_latest.md（Generated 2026-04-25 23:47:54）
    - Linux: log_bench_report_linux_current.md（Generated 2026-04-25 23:48:22）
  - 标准基线
    - Windows: log_bench_windows_latest.md（Generated 2026-04-25 18:24:53）
    - Linux: log_bench_linux_latest.md（Generated 2026-04-25 18:25:24）
  - Compare 基准（当前）
    - Windows: log_bench_compare_latest.md（Generated 2026-04-25 23:49:05）
    - Linux: log_bench_compare_report_linux_current.md（Generated 2026-04-25 23:49:53）
  - Compare 基线
    - Windows: log_bench_compare_windows_latest.md（Generated 2026-04-25 18:31:07）
    - Linux: log_bench_compare_linux_latest.md（Generated 2026-04-25 18:31:54）

说明：下表的 Delta(%) 以 us/log 计算，负值表示更快（提升），正值表示更慢（回退）。

## 3. 标准基准：当前 vs 基线

### 3.1 Windows（NEI）

#### Memory

| 场景 | 当前 us/log | 基线 us/log | Delta(%) |
|---|---:|---:|---:|
| Log Info | 0.3783 | 0.3525 | +7.32 |
| Log Warn | 0.3478 | 0.3513 | -1.00 |
| Log Error | 0.3418 | 0.3646 | -6.25 |
| Log with Formatting | 0.5572 | 0.5919 | -5.86 |
| Log Info (literal) | 0.3378 | 0.2997 | +12.71 |
| Log Verbose | 0.3647 | 0.3786 | -3.67 |
| Log Verbose (literal) | 0.3507 | 0.3212 | +9.18 |

#### File

| 场景 | 当前 us/log | 基线 us/log | Delta(%) |
|---|---:|---:|---:|
| File Log Info | 0.4106 | 0.4260 | -3.62 |
| File Log Warn | 0.4197 | 0.4237 | -0.94 |
| File Log Error | 0.4891 | 0.4214 | +16.07 |
| File Log with Formatting | 0.4729 | 0.4658 | +1.52 |
| File Log Verbose | 0.4243 | 0.4269 | -0.61 |
| File Log Info (literal) | 0.4053 | 0.3560 | +13.85 |
| File Log Verbose (literal) | 0.4187 | 0.3768 | +11.12 |

观察：Windows 结果为“有升有降”，大多数变化在可接受波动区间；但 File Error 与 literal file 两项存在明显回退，应继续跟踪。

### 3.2 Linux（NEI）

#### Memory

| 场景 | 当前 us/log | 基线 us/log | Delta(%) |
|---|---:|---:|---:|
| Log Info | 0.2101 | 0.2264 | -7.20 |
| Log Warn | 0.2179 | 0.2358 | -7.59 |
| Log Error | 0.2051 | 0.2257 | -9.13 |
| Log with Formatting | 0.3369 | 0.3337 | +0.96 |
| Log Info (literal) | 0.2270 | 0.2604 | -12.83 |
| Log Verbose | 0.2197 | 0.2323 | -5.42 |
| Log Verbose (literal) | 0.2310 | 0.2413 | -4.27 |

#### File

| 场景 | 当前 us/log | 基线 us/log | Delta(%) |
|---|---:|---:|---:|
| File Log Info | 1.1394 | 0.9621 | +18.43 |
| File Log Warn | 1.1470 | 1.0208 | +12.36 |
| File Log Error | 1.0433 | 1.1401 | -8.49 |
| File Log with Formatting | 1.1450 | 1.1935 | -4.06 |
| File Log Verbose | 1.0275 | 1.1025 | -6.80 |
| File Log Info (literal) | 0.9983 | 1.0433 | -4.31 |
| File Log Verbose (literal) | 0.9622 | 1.0586 | -9.11 |

观察：Linux memory 场景整体提升明显；file 场景分化较大，Info/Warn 回退突出，建议将 file sync 参数与存储介质状态纳入控制变量后复测。

## 4. Compare 基准：当前 vs 基线（关键项）

### 4.1 Windows compare

| 场景 | 当前 us/log | 基线 us/log | Delta(%) |
|---|---:|---:|---:|
| [NEI] memory simple | 0.3588 | 0.3525 | +1.79 |
| [NEI] memory multi | 0.5770 | 0.6069 | -4.93 |
| [NEI] memory literal | 0.3195 | 0.2830 | +12.90 |
| [NEI] file simple | 0.4205 | 0.4410 | -4.65 |
| [NEI] file multi | 0.5078 | 0.4989 | +1.78 |
| [NEI] file llog_literal | 0.4250 | 0.3663 | +16.02 |
| [NEI] sync simple | 1.1203 | 0.9304 | +20.41 |
| [NEI] sync multi | 0.9213 | 0.9530 | -3.33 |
| [NEI] strict multi | 8.8937 | 11.7137 | -24.07 |

观察：Windows compare 呈混合变化；strict multi 明显提升，literal file 与 sync simple 回退需要继续追踪。

### 4.2 Linux compare

| 场景 | 当前 us/log | 基线 us/log | Delta(%) |
|---|---:|---:|---:|
| [NEI] memory simple | 0.1758 | 0.1874 | -6.19 |
| [NEI] memory multi | 0.3106 | 0.2996 | +3.67 |
| [NEI] memory literal | 0.2054 | 0.2138 | -3.93 |
| [NEI] file simple | 1.0831 | 1.0313 | +5.02 |
| [NEI] file multi | 1.1405 | 1.0344 | +10.26 |
| [NEI] file llog_literal | 1.1959 | 1.0299 | +16.12 |
| [NEI] sync simple | 1.7696 | 1.4652 | +20.77 |
| [NEI] sync multi | 1.6446 | 1.6206 | +1.48 |
| [NEI] strict simple | 147.8760 | 143.4480 | +3.09 |

观察：Linux compare memory 仍有竞争力；file/sync 类场景多数回退。结合 strict 场景绝对值较高，推测与 IO 路径/宿主负载相关性强。

## 5. 横向结论（NEI vs spdlog）

- Windows memory：NEI 在 simple 场景快于 spdlog，multi 与 literal 场景不稳定，需按子场景判断。
- Linux memory：NEI 在 simple/literal/vlog_literal 场景显著领先。
- per-call flush over async：spdlog 在 Windows/Linux 均明显占优，NEI 仍有优化空间。
- strict sync flush：两者均受同步刷盘成本支配；该场景对设备和系统调度极端敏感。

## 6. 风险与可重复性说明

- 当前 Linux 数据来自 WSL 环境，file 与 strict/sync 场景对宿主磁盘状态和后台负载敏感。
- 标准差在少数场景偏大（例如 sync/strict/literal file），解读时应以趋势而非单点绝对值为主。
- 建议将“内存场景”和“文件场景”拆分验收阈值，避免误判。

## 7. 建议的后续动作

1. 对 Linux file info/warn、compare 的 NEI sync simple 做定向复测（5~10 runs）。
2. 固化 file sink 参数矩阵（flush_interval、batch_buf、strict sync）并建立场景映射。
3. 以本报告为当前新基线继续追踪；后续每次改动至少给出 memory+file 的双平台 Delta 表。
