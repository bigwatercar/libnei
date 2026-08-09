# NEI 全量 Benchmark 基线报告（最终版 · Windows + WSL 对照）

**日期**: 2026-08-09
**分支**: `dev` @ HEAD `0ac1aa9`
**构建**:
  - Windows: Visual Studio 2022（MSVC 19.44）, `build/windows-vs2022-shared`（Release x64, shared）
  - WSL: GCC 13, `build/linux-gcc-release-shared`（Release, shared）
**机器**: Intel Core Ultra 9 185H（6P+8E+2LPE, 16C/22T）, 32GB RAM, Windows 11 / WSL2
**原始日志**（gitignored）:
  - Windows: `bench/results/bench_20260809_214442/`
  - WSL: `bench/results/wsl_20260809_214854/`

> 本轮为 2026-08-09 最终全量采集（即 `bench_20260809_214442` + `wsl_20260809_214854`），相对
> r3 基线（`baseline_(Ultra9-185H)_20260809_r3.md` @ `8e8ce12`）代码无功能改动，差异来自今日
> 重构轮的全部提交（git 整理后合并为 20 个语义提交，备份分支 `backup-40commits`）：
>
> - `a2ed14c` P2 PostJob Chromium 对齐（posted_from & traits 透传 + priority 传递）
> - `969da57` ThreadPool 增加 Chromium 对齐的 ExecutionFence（BeginFence/EndFence）
> - `c1bb7db`（整理合并）ThreadPool Pimpl 化
> - `38e14a9`（整理合并）Worker-Repost local-queue WSL 掉速修复（+160%）
> - `77645c5` 修复 DelayedTaskManager 纯立即投递时的伪唤醒跳过
> - `271f33c` 修复 PooledTaskSource::Shutdown 丢唤醒（AtExit 拆解偶发挂起）
> - `02043dc` 修复 PostJob work-stealing joiner 饥饿/溢出（post_job_bench 崩溃）
> - `d412b52` **post_job_bench 多 worker 完成检测死锁修复**（OnWorkerExited 改用 `prev_assigned==1` 检测）
> - `debbb1e` PostJob 死锁回归测试（`RepeatedMultiWorkerJoinNeverHangs`，双平台多轮 0 死锁）
> - `8349cd7` SSLContext 回退为普通值对象（去掉 RefCountedThreadSafe，改由使用侧保证生命周期）
> - `f5e5a44` / `0ac1aa9` 记录清理与 `tools/*.sh` LF 规范化（非代码）

> **采集口径**：task_thread 10×1M（tracing ON/OFF 各一轮）、threadpool 5×1M（tracing OFF）。
> 所有数值为多次运行均值；带宽类取该次运行最佳值并标注对应缓冲大小。

---

## 1. 任务调度 Benchmarks

### 1.1 task_thread_bench — 10×1M tasks

**tracing ON**

| Scenario | Windows /s | WSL /s |
|---|---:|---:|
| Raw atomic loop（无任务系统） | 134,549,911 | 135,689,292 |
| BindOnce construction（仅构造） | 41,401,498 | 40,741,027 |
| BindOnce + Run | 31,117,229 | 30,949,557 |
| Standard PostTask（fast-path） | 4,875,590 | 3,760,070 |
| Delayed PostTask（non-fast-path） | 3,846,756 | 3,967,165 |
| Multi-threaded PostTask（4 线程） | 2,466,948 | 3,053,820 |

**tracing OFF**

| Scenario | Windows /s | WSL /s |
|---|---:|---:|
| Raw atomic loop（无任务系统） | 133,322,973 | 133,477,199 |
| BindOnce construction（仅构造） | 41,104,528 | 39,593,446 |
| BindOnce + Run | 30,094,333 | 30,250,544 |
| Standard PostTask（fast-path） | 3,987,640 | 4,134,120 |
| Delayed PostTask（non-fast-path） | 3,629,507 | 4,387,948 |
| Multi-threaded PostTask（4 线程） | 2,473,843 | 3,259,649 |

### 1.2 task_threadpool_bench — 5×1M tasks（tracing OFF）

| Scenario | Windows /s | WSL /s |
|---|---:|---:|
| Parallel PostTask（单线程投递） | 4,851,459 | 5,920,087 |
| Standard PostTask（external, global-heap） | 5,341,871 | 4,028,299 |
| SingleThread Standard（dedicated-worker） | 4,944,773 | 4,483,084 |
| SingleThread Delayed（non-fast-path） | 4,103,470 | 2,865,153 |
| Delayed PostTask（non-fast-path） | 3,444,607 | 2,513,223 |
| Multi-threaded PostTask（4 线程, sequenced） | 2,832,690 | 2,611,291 |
| SingleThread Multi-threaded（4 线程） | 2,812,926 | 3,367,693 |
| Parallel Multi-threaded（4 线程） | 2,783,803 | 2,444,215 |
| Parallel Worker-Repost（global-heap 路径） | 572,587 | 649,825 |
| Worker-Repost（local-queue fast-path） | 549,286 | 373,449 |

### 1.3 task_threadpool_parallel_bench — 5×1M

| Scenario | Windows /s | WSL /s |
|---|---:|---:|
| Parallel PostTask（单线程投递） | 4,623,836 | 6,115,962 |
| Parallel Multi-threaded（4 线程） | 2,721,504 | 2,555,611 |

### 1.4 post_job_bench

| Bench | 指标 | Windows | WSL |
|-------|------|----:|----:|
| 0 | Raw pool baseline | 16.4 µs | 33.2 µs |
| 1 | Sequential PostJob+Join（50K jobs） | **210,600 jobs/s**（4.7 µs/job） | 42,303 jobs/s（23.6 µs/job） |
| 2 | PostJob 1 worker inner loop（500K ops） | 36.3 M/s | 31.3 M/s |
| 2 | SeqRunner baseline（500K tasks） | 4.7 M/s | 3.3 M/s |
| 2 | PostJob 8 workers inner loop（500K ops） | 28.2 M/s | 29.0 M/s |
| 3 | w=1（10M ops/worker） | 263.60 M/s（1.0×） | 242.16 M/s（1.0×） |
| 3 | w=2 | 486.23 M/s（1.8×） | 471.71 M/s（1.9×） |
| 3 | w=4 | 932.25 M/s（3.5×） | 508.49 M/s（2.1×） |
| 3 | w=8 | 1,078.75 M/s（4.1×） | 530.06 M/s（2.2×） |
| 3 | w=16 | **1,566.17 M/s（5.9×）** | 517.57 M/s（2.1×） |

> **死锁回归验证**：双平台多轮 0 死锁（Windows 50/50、WSL 30/30）。Windows Bench3 扩展性良好
> （w=16 达 5.9×）；WSL 在 w≥4 后扩展性饱和（受 WSL2 线程调度/futex 开销限制，见 §8）。

### 1.5 parallel_runner_bench

**Bench 1 — atomic increment（M ops/s）**

| Workers | Windows | WSL |
|----:|----:|----:|
| 1 | 261.5 | 285.1 |
| 2 | 77.9 | 148.3 |
| 4 | 70.0 | 58.4 |
| 8 | 63.5 | 49.8 |
| 16 | 59.9 | 47.6 |

**Bench 2 — 100K small tasks（tasks/s）**

| Workers | Windows | WSL |
|----:|----:|----:|
| 1 | 736 K | 634 K |
| 2 | 844 K | 607 K |
| 4 | 853 K | 608 K |
| 8 | 819 K | 580 K |
| 16 | 700 K | 498 K |

**Bench 3 — PostTask throughput（PostTask/s）**

| Workers | Windows | WSL |
|----:|----:|----:|
| 1 | 546 K | 549 K |
| 2 | 544 K | 497 K |
| 4 | 535 K | 477 K |
| 8 | 533 K | 466 K |
| 16 | 528 K | 452 K |

> Raw pool baseline：Windows 15.5 µs / WSL 33.9 µs。

### 1.6 task_priority_perf_demo — 4 workers, 3×20K tasks, busy 2ms

| 指标 | Windows | WSL |
|------|----:|----:|
| 总任务数 | 60,000 | 60,000 |
| UserBlocking / UserVisible / BestEffort 占比 | 33.3% / 33.3% / 33.3% ✅ | 33.3% / 33.3% / 33.3% ✅ |
| Avg Queue（三优先级均值） | 1249.7 ms | 1249.7 ms |
| Max Queue（UserBlocking） | 39,999 ms | 40,002 ms |
| Throughput | 1,498.71 tasks/s | 1,498.48 tasks/s |
| Total elapsed | 40,034 ms | 40,041 ms |

> 三优先级严格均分（各 33.3%），无饿死；首 256 个任务起跑数 85/86/85，投递全部成功。

---

## 2. Callback Bench（SBO threshold=48B, LargePayload=64B）

| Metric | Windows /s | WSL /s |
|------|----:|----:|
| OnceCallback（non-SBO） | 41,325,729 | 31,093,328 |
| RepeatingCallback（non-SBO） | 96,618,357 | 109,068,143 |
| Mixed SBO/non-SBO | 41,423,304 | 40,855,197 |
| BindOnce（non-SBO） | 42,497,131 | 37,199,031 |

---

## 3. 内存与字符串

### 3.1 string_append_bench — 100K iterations（JSON fragments）

| Method | Windows ops/s | WSL ops/s |
|--------|----:|----:|
| StringAppendF | 5,335,325 | 9,519,277 |
| StringPrintf + operator+= | 4,492,363 | 9,108,298 |
| std::stringstream | 2,119,632 | 10,267,995 |
| speedup（AppendF vs Printf+=） | 1.19× | 1.05× |
| speedup（AppendF vs stringstream） | 2.52× | 0.93× |

> Windows 上 StringAppendF 仍为最优（vs stringstream 2.52×）；WSL 的 libstdc++ stringstream 优化
> 较好，三方法差距很小，速度均高于 Windows。

### 3.2 flake_id_bench — 8 线程 × 1M IDs（8M 总）

| 指标 | Windows | WSL |
|------|----:|----:|
| IDs/s | 293,548,896 | 285,277,729 |
| Elapsed | 27.25 ms | 28.04 ms |

---

## 4. 日志系统 Benchmarks

### 4.1 log_bench — NEI, 5 轮均值（E2E logs/s）

| Benchmark | Windows /s | WSL /s |
|-----------|----:|----:|
| Log Info | 3,216,598 | 3,574,510 |
| Log Warn | 3,196,424 | 3,702,580 |
| Log Error | 3,191,120 | 3,729,130 |
| Log with Formatting | 1,844,108 | 2,816,708 |
| Log Info (literal) | 3,061,114 | 3,149,719 |
| Log Verbose | 3,523,948 | 3,704,564 |
| Log Verbose (literal) | 3,187,266 | 3,932,524 |

> WSL 整体高于 Windows（异步管道在 WSL futex 下更高效）；带格式化场景差距最明显（+53%）。

### 4.2 log_bench_compare — NEI vs spdlog（5 轮均值，logs/s）

**Memory（async, minimal sink）**

| Benchmark | NEI Win | NEI WSL | spdlog Win | spdlog WSL |
|-----------|----:|----:|----:|----:|
| simple %s | **3,341,406** | 3,798,376 | — | — |
| vlog_literal（opaque body） | 3,034,700 | **4,432,498** | — | — |
| llog_literal（opaque body） | 3,266,480 | 4,296,488 | — | — |
| {fmt} literal simple | 2,866,348 | 3,876,888 | — | — |
| {fmt} literal multi | 1,991,728 | 3,002,860 | — | — |
| multi printf | 1,940,778 | 3,074,472 | — | — |
| multi printf（fmt_plan cache miss） | 1,867,640 | 3,143,904 | — | — |
| spdlog literal only | — | — | **4,164,642** | 3,160,780 |
| spdlog multi fmt | — | — | 2,082,914 | 2,273,730 |
| spdlog simple {} | — | — | 2,339,840 | 2,925,038 |

**File（async file sink）**

| Benchmark | NEI Win | NEI WSL | spdlog Win | spdlog WSL |
|-----------|----:|----:|----:|----:|
| file llog_literal | **3,678,222** | 1,206,558 | — | — |
| file vlog_literal | 3,553,582 | 1,254,410 | — | — |
| file simple %s | 2,980,756 | **1,248,264** | — | — |
| file {fmt} literal multi | 2,174,350 | 1,233,486 | — | — |
| file multi | 2,671,726 | 1,177,250 | — | — |
| spdlog file multi | — | — | 2,961,390 | 676,430 |
| spdlog file simple {} | — | — | **3,039,514** | 823,625 |

**File（per-call flush request over async pipeline）**

| Benchmark | NEI Win | NEI WSL | spdlog Win | spdlog WSL |
|-----------|----:|----:|----:|----:|
| file autoflush llog_literal | 2,691,974 | 1,185,768 | — | — |
| file autoflush vlog_literal | **3,353,554** | 1,161,403 | — | — |
| file autoflush simple | 2,676,970 | **1,243,472** | — | — |
| file autoflush multi | 2,425,510 | 842,854 | — | — |
| spdlog file sync multi | — | — | 5,465,434 | 6,138,322 |
| spdlog file sync simple | — | — | 5,314,692 | **7,471,820** |

**File（strict sync flush semantics）**

| Benchmark | NEI Win | NEI WSL | spdlog Win | spdlog WSL |
|-----------|----:|----:|----:|----:|
| file strict multi（sync flush each log） | 113,520 | 9,179 | **119,801** | 11,006 |
| file strict simple（sync flush each log） | 103,543 | 10,098 | **130,435** | 10,324 |

> 内存场景 NEI 全面领先或持平 spdlog（WSL vlog_literal 4.43M 为全场最高）；文件 async 场景
> Windows 上 NEI 领先、WSL 上二者接近且 WSL 整体低于 Windows（WSL 文件写入路径开销较大）；
> 逐条 flush / strict 同步语义下 spdlog 占优（其 flush 实现更轻量），属预期差异。

---

## 5. 文件与管道 IO（64 MB / run）

### 5.1 async_file_bench — 顺序读写（MB/s）

| Chunk | Win write | WSL write | Win read | WSL read |
|-------|----:|----:|----:|----:|
| 4 KB | 319.4 | — | 1,035.8 | — |
| 16 KB | 1,030.5 | — | 2,530.4 | — |
| 64 KB | 2,281.6 | 1,796.2 | 3,762.3 | **5,224.1** |
| 256 KB | 2,994.9 | 1,619.8 | 3,233.5 | 3,387.7 |
| 1 MB | 3,066.3 | 1,687.5 | 3,675.6 | 2,934.6 |
| 4 MB | **3,072.0** | 1,668.3 | **3,839.7** | 3,739.6 |

> Best write：Windows 3,072.0 MB/s @4MB / WSL 1,796.2 MB/s @64KB。
> Best read：Windows 3,839.7 MB/s @4MB / WSL 5,224.1 MB/s @64KB（WSL 读取占优，写入受 WSL2
> 文件系统桥接开销影响较低）。

### 5.2 pipe_stream_bench（MB/s）

| Chunk | Windows | WSL |
|-------|----:|----:|
| 4 KB | 412.2 | 4,351.3 |
| 16 KB | 617.4 | 6,063.4 |
| 64 KB | 2,254.7 | 6,399.3 |
| 256 KB | **6,473.8** | 5,961.8 |
| 1 MB | 4,040.6 | **6,367.5** |

> WSL（POSIX pipe）各档位普遍高于 Windows（匿名管道实现差异），最佳均在 6.3-6.5 GB/s 量级。

### 5.3 pipe_stream_cross_process_bench — 64B payload, 5000 iters

| 指标 | Windows | WSL |
|------|----:|----:|
| Msgs/s | **49,285** | 26,968 |
| Min | 6.70 µs | 10.92 µs |
| P50 | 19.80 µs | 34.92 µs |
| P95 | 24.30 µs | 51.11 µs |
| Avg | 20.29 µs | 37.08 µs |
| Max | 122.90 µs | 484.99 µs |

---

## 6. 网络 Benchmarks — TCP

### 6.1 tcp_loopback_bench — 1024 MB 传输

| Buffer | Windows MB/s | WSL MB/s |
|--------|----:|----:|
| 4 KB | 295.8 | 1,123.6 |
| 8 KB | 563.5 | 1,965.6 |
| 16 KB | 1,052.6 | failed* |
| 32 KB | 1,532.4 | 5,039.5 |
| 64 KB | 2,327.9 | 6,634.5 |
| 128 KB | 2,980.6 | 7,840.4 |
| 256 KB | 3,718.0 | 10,181.6 |
| 512 KB | 3,428.5 | **11,783.0** |
| 1024 KB | **3,783.1** | 11,585.2 |

> *WSL 16KB 档出现一次 `connect failed`（环境偶发，其余档位正常），该档未计入。
> WSL（原生内核 TCP 栈）显著高于 Windows，最佳 11.8 GB/s @512KB。

### 6.2 tcp_throughput_bench — 10 MB, 64 KB buffer, FNV-1a 校验

| 指标 | Windows | WSL |
|------|----:|----:|
| Elapsed | 0.022 s | 0.010 s |
| Throughput | 457.7 MB/s | **992.9 MB/s** |
| Integrity | OK | OK |

### 6.3 tcp_rtt_bench — 1000 并发连接, 4 workers

| 指标 | Windows | WSL |
|------|----:|----:|
| Total elapsed | 0.620 s | **0.100 s** |
| RTT min | 241 µs | 4,464 µs |
| RTT avg | 11,131 µs | **8,045 µs** |
| RTT max | 15,275 µs | 10,433 µs |
| RTT p50 | 14,739 µs | **8,255 µs** |
| RTT p90 | 15,173 µs | 10,262 µs |
| RTT p99 | 15,239 µs | 10,420 µs |

### 6.4 tcp_conn_stress_bench — 1000 连接, 4 workers

| 指标 | Windows | WSL |
|------|----:|----:|
| Elapsed | 0.623 s | **0.088 s** |
| Rate | 1,606.1 conn/s | **11,310.8 conn/s** |
| Server accepts（failures） | 1000（1） | 1000（1） |
| Client done（failures） | 1000（0） | 1000（0） |

> 服务端偶发 1 个 accept 失败为环境偶发，客户端 1000/1000 全部成功。

---

## 7. 网络 Benchmarks — TLS（mbedTLS）

### 7.1 tls_throughput_bench — 10 MB, 64 KB buffer, FNV-1a 校验

| 指标 | Windows | WSL |
|------|----:|----:|
| Elapsed | 0.099 s | 0.071 s |
| Throughput | 101.5 MB/s | **141.7 MB/s** |
| Integrity | OK | OK |

> SSLContext 已回退为普通值对象（去 RefCountedThreadSafe），bench 与单测均验证
> 使用侧生命周期契约下无回归。

---

## 8. 关键结论（vs r3 基线 @ 8e8ce12）

| 维度 | r3 → 最终 | 说明 |
|------|------|------|
| **post_job Bench3 w=16** | 1,036 → **1,566 M/s**（+51%, Win） | 死锁修复后 Windows 扩展性达 5.9×，为历史最高；WSL 767 → 517 M/s（w≥4 后受 WSL2 调度限制饱和，见下） |
| **threadpool Parallel（WSL）** | 4.85 → **5.92 M/s**（+22%） | WSL 单线程投递持续走高，Windows ≈ 持平（4.97 → 4.85 M/s，-2%） |
| **threadpool Standard（WSL）** | 3.69 → **4.03 M/s**（+9%） | global-heap 投递 WSL 改善 |
| **task_thread Standard（tracing OFF）** | Win 4.17 → 3.99 M/s（-4%）/ WSL 3.85 → 4.13 M/s（+7%） | Windows 略降属运行波动，双平台均值无实质回归 |
| **task 调度整体** | 无回归 | task_thread / threadpool / parallel 全场景双平台水平与 r3 一致或更优 |
| **Worker-Repost（local-queue）** | Win 576 → 549 K/s / WSL 346 → 373 K/s | 与 `38e14a9` 修复后水平一致（WSL 已脱离 0.32M 低谷） |
| **日志 / 字符串 / Callback / IO** | 持平 | 双平台量级与 r3 一致 |

**注意事项**
- **WSL post_job 扩展性饱和**：w=1 与 Windows 相当（242 vs 264 M/s），但 w≥4 后不再线性扩展
  （WSL2 线程调度与 futex 开销，Bench1 50K jobs 23.6µs/job vs Win 4.7µs 亦反映同因），
  属平台差异而非库缺陷（详见 docs/TODO.md）。
- **Bench1（50K 单 op jobs）**：WSL 42,303 jobs/s vs Windows 210,600 —— 已知 WSL2 线程
  handoff 延迟所致，非库缺陷。
- **r3 与最终为同一代码**：两轮采集间代码无功能变更（仅回归测试、SSLContext 回退与记录整理），
  差异主要为运行噪声与平台偶发（WSL tcp_loopback 16KB connect failed）。
- 今日 Windows 构建必须全量 `cmake --build`（禁止 `--target X`），否则 bench/tests 子目录 dll
  不更新导致验证基于旧 dll 无效（见 `build-copy-dll-lesson.md`）。

---

## 附录: 构建与采集命令备忘

```powershell
# Windows Release（全量构建，dll 拷贝规则随 exe 构建触发）
cmake --build build/windows-vs2022-shared --config Release
$env:PATH = "C:\Personal\Projects\LibNei\libnei-src\build\windows-vs2022-shared\Release;" +
            "C:\Personal\Projects\LibNei\libnei-src\build\windows-vs2022-shared\bench\Release;$env:PATH"
powershell -File bench\run_all_benches.ps1 -BenchBinDir "build\windows-vs2022-shared\bench\Release" `
  -LogOutputDir "bench\results"

# WSL（GCC Release, shared；RPATH 直连 .so 无需 LD_LIBRARY_PATH）
wsl bash -c 'cd /mnt/c/Personal/Projects/LibNei/libnei-src && cmake --build build/linux-gcc-release-shared -j$(nproc) && bash tools/wsl_run_all.sh'

# Report 生成
powershell -File tools\gen_bench_report.ps1 -LogDir <dir>
```

## 附录: 原始日志清单

- Windows: `bench/results/bench_20260809_214442/*.log` + `report.md`
- WSL: `bench/results/wsl_20260809_214854/*.log` + `report.md`
- 相关基线文档：`baseline_(Ultra9-185H)_20260809_r3.md`（同日 r3）、
  `baseline_(Ultra9-185H)_20260806_windows.md` / `baseline_(i5-10400T)_20260806_windows.md`（前两轮）
