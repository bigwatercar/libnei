# NEI Log Bench 结果解读与后续对比基线（2026-04-26）

## 1. 数据范围

- Windows 10轮（Release）
  - 原始目录：`bench/results/windows_release_10run_20260426_003513`
  - 归一化报告：`bench/results/windows_release_10run_20260426_003513/benchmark_report_windows_release_10run_normalized.md`
- Linux/WSL 10轮（Release）
  - 原始目录：`bench/results/linux_wsl_release_10run_20260426_005713`
  - 归一化清洗报告：`bench/results/linux_wsl_release_10run_20260426_005713/benchmark_report_linux_wsl_release_10run_normalized_clean.md`

注：Linux 报告对 1 个非正值异常样本做了清洗（保留原始日志可追溯）。

## 2. 关键结论（先看）

1. 内存路径（非文件）在 Linux/WSL 明显更快。
   - 例如 `Log Info`：Windows 0.3039 us/log，Linux 0.2063 us/log（Linux 约快 32.1%）。
2. 文件路径（异步文件 sink）在 Linux/WSL 明显更慢。
   - 例如 `File Log Info`：Windows 0.3914 us/log，Linux 0.7772 us/log（Linux 约慢 98.6%）。
3. NEI vs spdlog 的胜负和场景强相关。
   - 内存简单/格式化：Linux 上 NEI 优势更明显。
   - per-call flush（file sync）场景：两平台都明显是 spdlog 更快。
4. 稳定性整体可用，但 Linux 文件路径抖动更高。
   - Linux 文件场景 CV 常见在 6% 到 16%；Windows 同类多数在 1.5% 到 4.8%。

## 3. 跨平台对照（NEI 主路径）

| 场景 | Windows us/log | Linux us/log | 变化（Linux 相对 Windows） |
|---|---:|---:|---:|
| Log Info | 0.3039 | 0.2063 | -32.1% |
| Log with Formatting | 0.5153 | 0.2988 | -42.0% |
| File Log Info | 0.3914 | 0.7772 | +98.6% |
| File Log with Formatting | 0.4303 | 0.8158 | +89.6% |

说明：负号表示 Linux 更快；正号表示 Linux 更慢。

## 4. NEI vs spdlog 对比解读（同平台）

### 4.1 Windows（10轮）

| 场景对 | spdlog/NEI | 结论 |
|---|---:|---|
| simple（内存） | 1.5931 | NEI 更快 |
| multi（内存格式化） | 0.8826 | spdlog 更快 |
| file simple（异步） | 0.9322 | spdlog 小幅更快 |
| file multi（异步） | 0.8556 | spdlog 更快 |
| file sync simple（每条 flush 请求） | 0.2351 | spdlog 显著更快 |

### 4.2 Linux/WSL（10轮 clean）

| 场景对 | spdlog/NEI | 结论 |
|---|---:|---|
| simple（内存） | 2.1729 | NEI 显著更快 |
| multi（内存格式化） | 1.8301 | NEI 显著更快 |
| file simple（异步） | 1.7249 | NEI 显著更快 |
| file multi（异步） | 1.9056 | NEI 显著更快 |
| file sync simple（每条 flush 请求） | 0.1244 | spdlog 显著更快 |
| file strict simple（同步 flush） | 1.0029 | 基本持平（NEI 略快） |

## 5. 诊断数据解读（NEI）

1. `ring_hwm`
   - Windows 异步路径接近 1026；Linux 异步路径接近 257。
   - 表示两平台构建/运行时的队列深度上限或调度行为不同，后续跨平台对比不能只看绝对值，需要看同平台趋势。
2. `producer_spins`
   - 在格式化重场景明显升高（两平台一致），可作为后续优化格式化路径的重要佐证指标。
3. `flush_wait_loops`
   - per-call sync 场景接近 0 或很低，但 latency 仍高，说明瓶颈主要不在等待循环计数，而在 flush 语义本身及 I/O 路径。

## 6. 后续对比建议（固定口径）

后续每次迭代建议固定以下“主 KPI”进行同表对比：

| 维度 | 场景 | 指标 |
|---|---|---|
| 内存快路径 | `Log Info` / `[NEI] simple %s` | us/log, logs/s, CV% |
| 格式化开销 | `Log with Formatting` / `[NEI] multi printf` | us/log, producer_spins |
| 文件异步 | `File Log Info` / `[NEI] file simple %s` | us/log, logs/s, CV% |
| 文件强同步 | `[NEI] file sync simple` / `[NEI] file strict simple` | us/log, CV% |
| 对标关系 | 对应 spdlog pair | spdlog/NEI 比值（>1 NEI胜，<1 spdlog胜） |

建议判定阈值：

- 性能收益：`|变化| >= 5%` 视为有效收益或回退。
- 稳定性风险：`CV% > 10%` 需至少补采样一次并复核环境噪声。

## 7. 作为基线的结论

- Windows 基线：`windows_release_10run_20260426_003513`
- Linux/WSL 基线：`linux_wsl_release_10run_20260426_005713`

后续报告可直接沿用本文表头与阈值，增量记录“相对本基线”的变化百分比即可。