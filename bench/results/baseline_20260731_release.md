# NEI 全量 Benchmark 基线报告

**日期**: 2026-07-31
**分支**: `post_job_implement` (HEAD: 16d50f2)
**构建**: Visual Studio 2022, `cmake --build build/win-rel --config Release`
**机器**: Intel Core Ultra 9 185H (6P+8E+2LPE, 16C/22T), Base 2.3GHz / Turbo ~5.1GHz, 32GB RAM, Windows 11 专业工作站版

---

## 1. 任务调度 Benchmarks

### 1.1 task_thread_bench — 100K tasks

| 场景 | Post | Total | ns/task | tasks/s |
|------|-----:|-----:|-------:|-------:|
| Standard fast-path | 12.6ms | 13.2ms | **125.8** | **7,947,988** |
| Delayed non-fast-path | 25.1ms | 30.9ms | 250.8 | 3,987,145 |
| Multi 4-thread | 37.8ms | 37.9ms | 377.9 | 2,646,518 |

### 1.2 task_threadpool_bench — 100K tasks

| 场景 | Post | Total | ns/task | tasks/s |
|------|-----:|-----:|-------:|-------:|
| Standard fast-path | 16.1ms | 20.1ms | 161.2 | 6,203,320 |
| Delayed | 30.9ms | 48.2ms | 308.7 | 3,239,265 |
| Multi 4-thread | 27.1ms | 29.4ms | 271.5 | 3,683,309 |
| Parallel 1-thread | 37.7ms | 37.8ms | 377.4 | 2,649,526 |
| Parallel 4-thread | 48.5ms | 48.5ms | 485.0 | 2,061,936 |

### 1.3 post_job_bench

| Bench | 指标 | 值 |
|-------|------|-----|
| 0 | Raw pool baseline | **14.2 µs** |
| 1 | Sequential PostJob+Join (50K) | **203,864 jobs/s** (4.9 µs/job) |
| 2 | PostJob 1 worker inner loop (500K ops) | **115.2 M/s** |
| 2 | SeqRunner baseline (500K tasks) | 5.3 M/s |
| 2 | PostJob 8 workers inner loop (500K ops) | 30.5 M/s |
| 3 | w=1 (10M ops/worker) | **253.2 M/s** |
| 3 | w=2 | 477.6 M/s (1.9×) |
| 3 | w=4 | 501.1 M/s (2.0×) |
| 3 | w=8 | 615.6 M/s (2.4×) |
| 3 | w=16 | **767.6 M/s** (3.0×) |

### 1.4 parallel_runner_bench

| Bench | 场景 | w=1 | w=2 | w=4 | w=8 | w=16 |
|-------|------|-----:|-----:|-----:|-----:|-----:|
| 1 | atomic increment | 137.5M/s | 141.1M/s | 122.0M/s | 60.1M/s | 60.1M/s |
| 2 | 100K small tasks | 2.61M/s | 2.65M/s | 2.55M/s | **2.80M/s** | 2.76M/s |
| 3 | PostTask throughput | 2.87M/s | 2.92M/s | **2.93M/s** | 2.93M/s | 2.91M/s |

---

## 2. 日志系统 Benchmarks

### 2.1 log_bench — 1M iterations, memory sink

| 测试 | E2E logs/sec | E2E µs/log |
|------|------------:|----------:|
| Log Info | 3,918,330 | 0.255 |
| Alt 2 fmts | 3,397,630 | 0.294 |
| Alt 3 fmts | 3,548,540 | 0.282 |
| Verbose Alt 2 fmts | 3,871,140 | 0.258 |
| C printf | 2,392,500 | 0.418 |
| {fmt} literal | 1,928,720 | 0.518 |
| C printf simple | **4,209,590** | 0.238 |
| {fmt} literal simple | 2,776,780 | 0.360 |
| Verbose C printf | 3,876,950 | 0.258 |
| Verbose {fmt} literal | 2,524,440 | 0.396 |

### 2.2 log_bench — 100K iterations, file/SSD sink

| 测试 | E2E logs/sec |
|------|------------:|
| File Log Verbose (literal) | **4,123,030** |
| File Log Info (literal) | 3,189,280 |
| File Log Error | 3,531,320 |
| File Log Verbose | 3,551,140 |
| File Log Warn | 2,918,430 |
| File Log Info | 2,879,440 |
| File with Formatting | 2,680,250 |
| File C printf | 1,798,720 |

### 2.3 log_bench_compare — NEI vs spdlog (1M memory / 100K file)

| 场景 | NEI | spdlog | NEI 优势 |
|------|-----:|-----:|:---:|
| **内存** simple %s | **4,287,520** | 2,135,160 | **2.0×** |
| 内存 literal vlog | **4,566,610** | 4,165,950 | +10% |
| 内存 multi printf | **2,491,550** | 2,339,120 | +7% |
| **文件** simple | **3,255,950** | 3,026,360 | +8% |
| 文件 vlog literal | **4,438,130** | — | — |
| 文件 multi | **3,219,470** | 2,661,910 | +21% |
| 文件 autoflush simple | 3,210,270 | **5,737,230** | spdlog +79% |
| 文件 strict sync simple | 123,013 | 130,904 | 持平 |

---

## 3. 网络 Benchmarks — TCP

### 3.1 tcp_loopback_bench — 1024MB 传输

| 缓冲 | 耗时 | 吞吐 |
|------|-----:|-----:|
| 4 KB | 5.79s | 177 MB/s |
| 16 KB | 1.54s | 665 MB/s |
| 64 KB | 0.54s | 1,900 MB/s |
| 128 KB | 0.34s | 3,042 MB/s |
| 256 KB | 0.33s | 3,142 MB/s |
| 512 KB | 0.33s | 3,083 MB/s |
| **1 MB** ★ | 0.32s | **3,251 MB/s** |

### 3.2 tcp_throughput_bench — 500MB, 双 IO 线程, FNV-1a 验证

| 缓冲 | 耗时 | 吞吐 |
|------|-----:|-----:|
| 64 KB (默认) | 1.40s | 357 MB/s |
| **512 KB** ★ | 0.69s | **724 MB/s** |

### 3.3 tcp_rtt_bench — 1000 并发连接

| 指标 | 值 |
|------|-----|
| 连接数 | 1000 |
| Workers | 4 |
| RTT min | 7,628 µs |
| RTT avg | 14,265 µs |
| RTT max | 20,047 µs |
| RTT p50 | **19,129 µs** |
| RTT p99 | 20,020 µs |
| 总耗时 | 0.651s |

### 3.4 tcp_conn_stress_bench — 1000 连接压力

| 指标 | 值 |
|------|-----|
| 连接数 | 1000 |
| Workers | 4 |
| 耗时 | 0.63s |
| 速率 | **1,588 conn/s** |
| 服务端失败 | 1 |

### 3.5 tcp_cross_bench — 跨系统 (Win ↔ WSL2)

> **实测 bench 输出**: `Direction : client → 127.0.0.1:9003`
> **`→` 指向服务端**（格式: `客户端 → 服务端`）。
> "客户端连接 IP:端口" 列为客户端 `--host` 参数指定的地址。

| 方向 (客户端 → 服务端) | 客户端连接 IP:端口 | 连接数 | 成功率 | 速率 |
|------|------|-----:|:---:|-----:|
| **WSL → Win** | `172.18.16.1:9002` (gateway) | 10,000 | 100% | **30,915 conn/s** |
| Win → WSL | `127.0.0.1:9001` (localhost NAT) | 10,000 | 100% | 4,356 conn/s |

---

## 4. 网络 Benchmarks — TLS (mbedTLS)

### 4.1 tls_throughput_bench — 500MB, 单线程, FNV-1a 验证

| 缓冲 | 耗时 | 吞吐 | TLS/TCP |
|------|-----:|-----:|:---:|
| 64 KB (默认) | 4.53s | 110 MB/s | — |
| **16 KB** ★ | 3.80s | **132 MB/s** | 18.2% |

> ★ 16KB 是 TLS 甜点——恰好填满一个 TLS 记录（16KB），零浪费。
> 参考 TCP 甜点 512KB / 724 MB/s。

---

## 5. 关键结论

| 维度 | 亮点 |
|------|------|
| **任务调度** | PostTask 125 ns/task (8M/s 线程), PostJob w=16 扩展 3× (768M/s) |
| **日志** | simple 路径 4.3M/s (spdlog 2×), literal 4.57M/s, 文件 4.44M/s |
| **TCP** | loopback 3,251 MB/s, throughput 724 MB/s (512KB), cross 30,915 conn/s |
| **TLS** | 132 MB/s (TLS/TCP=18.2%), mbedTLS 纯软件加密 |
| **跨系统** | WSL→Win 30,915 conn/s (gateway 直连, 10K 连接 0.32s) |

---

## 附录: 构建命令备忘

```powershell
# Windows Release (必须加 --config Release!)
cmake --build build/win-rel --config Release --target <target> -j

# WSL Release
cmake --build build/wsl-rel --target <target> -j$(nproc)

# 跨系统 bench
# WSL→Win: WSL client -> Windows gateway IP
# Win→WSL: Windows client -> 127.0.0.1 (WSL2 localhost forwarding)
```
