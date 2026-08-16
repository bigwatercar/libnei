# libnei — TODO & Roadmap

**Updated**: 2026-08-14

---

## HTTP 大数据渐进式传输重构（2026-08-13 规划）

**缺口**：HttpClient 上传/下载全量缓冲进 `std::string`（下载 `ResponseDelegate::OnBody` append、
上传 `SerializeRequest` 整包）。Server 已有双向流式（`AddStreamingRoute` write / `AddStreamingRequestRoute` read_body）。
解析器已支持增量 OnBody + chunked，Client 端补流式无需动解析核心。

**Phase 1 — Client 流式下载 ✅ 已落地 2026-08-13**（`HttpClient::SendStreaming`）：
- `ResponseHeadersCallback` / `BodyChunkCallback`（`on_headers` 先触发，`on_body` 逐块派发，`(nullptr,0,true)` 结束）
- `ResponseDelegate` 流式钩子：`OnHeadersComplete` 派发 headers（`status_provider` 从 parser 读数字码）、
  `OnBody` 直派、`OnMessageComplete` 派发 done；EOF 分支合成 done（read-until-close）
- 内存有界；无背压基线；keep-alive 需消费到 done
- 4 集成测试（分片/headers-first/chunked/keep-alive/close）+ 41 回归全过

**Phase 2 — Client 流式上传 ✅ 已落地 2026-08-13**（`HttpClient::SendBody`）：
- `RequestBodyProvider` 推流式 body；`SerializeRequest` 拆 headers-only
- 复用 write_queue 单在途写模式（`PumpUploadWrites`）；Content-Length 或 chunked 两种编码
- 2 集成测试（Content-Length 分块上传 / chunked 编码）+ 49 回归全过

**Phase 3 — 大文件便捷层 ✅ 已落地 2026-08-13**（`HttpFileTransfer`，`DownloadToFile` / `UploadFromFile`）：
- `DownloadToFile`：SendStreaming + AsyncFile 偏移写 + IOBufferPool 零拷贝；非 2xx 丢弃 body；
  全部写盘完成（body_done + pending_writes==0）后 CloseAsync 再回调 `(ok, bytes)`
- `UploadFromFile`：SendBody pull provider 逐块 ReadAsync（单块在途，内存有界）；回调携带响应体
- 2 集成测试（64KB 下载落盘 / 上传回显校验）+ net 145 回归全过

**Phase 4 — 服务端一致性 ✅ 已落地 2026-08-13**：
- 流式 write 增加零拷贝 IOBuffer 句柄 `write_io`（buffer 直达 socket 写队列，无 memcpy）；新增
  `HttpResponseWriter::SerializeChunkHeader` 支撑零拷贝分块帧（size 行 + 裸数据 + CRLF）
- 流式请求体缓冲 `sr_chunks` 由 `std::string` 改为 IOBuffer（IOBufferPool），且单独记录真实长度
  （`AcquireBuffer` 的 size() 是规格化桶大小 ≠ 请求长度；2026-08-14 已落地类型隔离：
  返回 `PooledIOBuffer` 不暴露 size()，仅 capacity()，误用变编译错误）
- 背压暂停读：缓冲超 256 KiB 高水位暂停 socket 读，handler 拉取到 64 KiB 低水位或阻塞等待时恢复；
  内存有界，快速消费 / 延迟消费均无死锁
- 2 集成测试（write_io 64KB 零拷贝响应 / 1MiB 延迟消费大上传背压）+ HTTP 集成 21 全过、
  net 全量 146 过 4 跳过（`ServerDestroyDuringTraffic` 当时判为环境敏感偶发；
  **2026-08-14 已确证为 Windows 真死锁并经 `3cd8036` 修复，见 Known Flaky 表**）

---

## 待办事项（未完成）

### P1

- **单线程 TaskRunner 队列调度开销优化** ✅ 已关闭 2026-08-10:

  **当前基线**（Ultra 9 185H, Release）：
  | 平台 | PostTask 延迟 | 吞吐 | vs 目标 4M/s |
  |------|:---:|------|:---:|
  | Windows | 229ns | 4.4M/s | ✅ 超标 10% |
  | WSL (GCC) | 251ns | 4.0M/s | ✅ 达标 |

  **已尝试的优化（全部记录了教训，见 repo memory）**：
  | 实验 | 收益 | 结论 |
  |------|:---:|------|
  | SequenceManager fast-path A/B | +8.8% | queue-lock + Take 仍主导 |
  | 回调拆分 (OnImmediateTaskPostedCallback) | Win -32% | 锁缓存同步反效果，已回退 |
  | Selector work-bit deferred flush | 多线程 +28% | 已落地 |

  **核心结论**：跨线程投递的真正瓶颈是每次投递（队列空时）的 pump wake
  （eventfd Signal + pump lock），而非队列结构或回调锁。当前数值已达标，
  进一步优化（批量提交/TLS flush）ROI 低，移至远期方向。

- **Parallel worker-repost 缺陷：`running_worker_count_` 负溢出 + 任务重复执行 + 死锁** ✅ 已修复 2026-08-08（`a01dd2a`）:

  **症状**：benchmark `RunWorkerRepostBenchmark(seed_count=4)` 在 parallel runner 上导致
  `g_executed_task_count` 远超 task_count（executed=220 for expected=100），随后崩溃
  (0xC0000005) 或死锁。sequenced runner 的 seed_count=1 无此问题。

  **根因**：`PooledTaskSource::ReEnqueueTaskQueue()` 的 Phase 2.2（TLS local WorkQueue 注入）
  对 parallel queue 也生效——worker 内 PostTask 时 parallel queue 被 push 到本地队列，
  绕过 `WillRunTask()` → `running_worker_count_` 缺少对应的 +1。本地队列分支处理时只调
  `DidProcessTask()`（fetch_sub(-1)），无配对 `WillRunTask()`（fetch_add(+1)），导致
  `running_worker_count_` 持续负溢出 → `DidProcessTask()` 饱和检查 `prev >= 256` 永远
  false → 队列从不重入全局堆 → 多 worker 并发无保护地 `ProcessTaskBatch` 同一队列 +
  本地队列重复 push → 任务放大执行 + 状态污染后续场景 → 死锁。

  **修复**：Phase 2.2 排除 parallel queue（`!queue->is_parallel()`），parallel queue
  始终走全局 shard heap + WillRunTask 槽位保护。改动 1 行（`pooled_task_source.cpp`）。
  Chromium 的 local work queue 也仅用于 sequenced runner，不适用于全局共享的 parallel
  queue。

### P2

- **唤醒路径性能回退（2026-08-15 bench 分析，待优化）** 📐:
  丢失唤醒修复（`9c6e54a`，Signal/Broadcast 持 `wait_lock_`）在唤醒握手密集场景有可测回退（同机 A/B）：
  - **WSL dedicated SingleThread 投递 -38~41%**（4.08M→2.5M/s）：pthread ERRORCHECK mutex 每次握手成本 ~150ns；
  - **Windows Parallel 单线程投递 -18.6%**（4.68M→3.81M/s）：每任务两次 notify 的 SRW 锁成本；
  - 其余场景（global-heap 标准投递、MT 4 线程、Worker-Repost 本地队列）在噪声带内（±5~10%）。
  跨日对比（vs 20260809 基线）额外受机器漂移（WSL 无负载亦 -10~30%）与 `ff5aa06` SharedWorker 变更干扰。
  已排除：计数器跳锁（睡眠者计数，对 ping-pong 场景无益，因每次唤醒时 worker 都在睡）、
  `alignas(64)` 缓存行隔离、每队列 WaitableEvent（POSIX Signal=eventfd syscall，更差 -48%）。
  **futex-flag（P2 原方案）已实测并排除（2026-08-15）**：dedicated 路径改造为单睡眠者
  futex/WaitOnAddress 标志后，WSL ping-pong 反而 -50%（post 侧 457ns vs nofix 245ns）：
  worker 每帖必睡时每次唤醒都要 futex_wake syscall（WSL2 ~250ns），而 glibc broadcast 的
  wrefs 检查常在“无等待者”时跳过 syscall，cv 版反而更便宜。
  **2026-08-15 系统审查（`docs/task_sync_review_20260815.md`）结论**：最优演进为 Chromium
  已验证的 **spin-then-park**（worker 睡眠前自旋，0→指数退避至 8ms；解锁全部后续优化，
  预计 dedicated 恢复到 nofix 水平）+ 事件化唤醒收敛 + 侵入式 TaskSource 状态。
  **下一步 = 阶段 1 spin-then-park**（验收：WSL dedicated ≥3.8M/s，TSan/全量不回归）。
  **阶段 1 已实施并验证失败（2026-08-15，已回退）**：实验矩阵：纯自旋（pause 指令在 WSL2
  触发 VM exit，-95%）、纯 load 自旋（-95%）、无锁 work-hint 自旋（-95%）、dedicated 独立
  唤醒通道去 herd（-43.4% 同会话）——全部达不到验收。根因：**每次唤醒的 pthread
  error-check 锁 + futex 握手在 WSL2 上 ≈200ns/帖**，任何机制都躲不开（唯一避开的 nofix
  正是靠无锁 broadcast = 丢唤醒 bug 本身）；自旋在 VM 下无收益甚至更差。
  结论：阶段 1 在当前锁/事件原语下不可行；真正的先决条件是**廉价的等待原语**（futex/
  WaitOnAddress 单字事件、非 error-check 锁、或 Windows 式 WaitableEvent），否则 worker
  每帖必睡的场景无解。已回退，保留 9c6e54a 简单正确修复。实验数据：`wsl_task_{spin,nospin,nofix2}`。
  相关 bench 数据：`bench/results/{bench_task_20260815_092618, wsl_task_20260815_084819}`（修复版）、
  `{bench_task_nofix_win, wsl_task_nofix}`（无锁 A/B）、`wsl_task_{fix2,counter,event,align,wakeflag}`（优化实验）。

- **HttpClientPool keep-alive 空闲连接 CLOSE_WAIT 堆积（2026-08-14 分析，待实施）** 📐:
  **风险**：keep-alive 连接 kIdle 时 socket 保留且无 pending read；对端（server）主动关闭空闲连接 →
  本地收 FIN 进 CLOSE_WAIT，但 `is_connected()` 只查 `state==kIdle && 有socket`（http_client.cpp:550）、
  `StartKeepAliveMonitor`（getsockopt SO_ERROR）检测不到优雅 FIN（CLOSE_WAIT 时 SO_ERROR=0）→
  CLOSE_WAIT 堆积直到复用/析构。池内受 `max_idle_per_endpoint=6` 限制有界；池外用户持有的 keep-alive
  HttpClient 无上限。
  **与死锁修复（3cd8036）的关系**：修复本身不引入 CLOSE_WAIT（本地主动 close → 无 CLOSE_WAIT），
  反而减少活动连接 CLOSE_WAIT（修复前 Windows 回环下对端 read 不被 FIN 完成而堆积）；但修复后
  server 更主动关闭，会**放大**空闲连接 CLOSE_WAIT 出现频率。属连接池既有缺口，非修复回归。
  **方案评价（结论）**：
  - ✅ 首选 方案2 idle timeout：Release 记时间戳，Acquire 惰性清理超时空闲 → 直击堆积、行业标准、无检测缺陷
  - 🥈 可选 方案1：Acquire 时 `recv(MSG_PEEK)` 复用前探活（返回 0=对端 FIN）→ 避免复用死连接；
    Windows overlapped socket 同步 recv 需先实测
  - ❌ 排除 方案3：StartKeepAliveMonitor（SO_ERROR）检测不到对端优雅 FIN
  状态：✅ 已实施（2026-08-14）。
  实现：
  - **Phase A idle timeout**：`HttpClientPool::SetIdleTimeout(TimeDelta)`（默认 30s，0/负=禁用）；
    `idle_clients` 改 `deque<IdleEntry{client, released_at}>`，Release 记 `TimeTicks::Now()`，
    Acquire 惰性清理过期/死连接（`Close()` 主动回收 CLOSE_WAIT socket）。
  - **Phase B liveness probe**：`TCPClientSocket::Peek()`（win `recv(MSG_PEEK)` / posix
    `recv(MSG_PEEK|MSG_DONTWAIT)`；0=对端FIN、>0=有pending数据丢弃、EAGAIN=存活）+ `TLSClientSocket::Peek()`
    透传 + `HttpClient::Peek()`；前置 FIONBIO 已落地（socket 创建即非阻塞，peek 永不阻塞）。
  - 池侧 `Acquire` 合并逻辑：`is_connected() && !expired && Peek()` 才复用，否则 `Close()`+丢弃。
  - 4 新集成测试（ExpiredNotReused / WithinWindowReused / Disabled / LivenessProbeDiscardsServerClosedIdleConnection，
    新增 `/respond-then-close` 路由模拟 server 关闭空闲连接）。
  验证：Windows Release 全量 **839/830 通过**（8 DNS 跳过，1 ChildProcess 偶发复跑过）；
  WSL net **126/125**（唯一失败 = 已知 ServerDoesNotCrashUnderFdPressure）。

- **PostJob 接口参数对齐 Chromium** ✅ 2026-08-09（`c09be98`）:
  `PostJob(from_here, traits, ...)` 此前被 `(void)` 丢弃、`CreateParallelTaskRunner(TaskTraits())`
  硬编码默认优先级。已修复：
  - ① `JobTaskSource` 构造接收 `Location`/`TaskTraits`，存 `posted_from_`/`traits_`；
  - ② `PostWorkers` 用 `PostTaskWithTraits(FROM_HERE, traits_, ...)` 透传 traits → worker 按
    traits 优先级执行（此前硬编码 USER_VISIBLE）；
  - ③ 提供 `posted_from()`/`traits()`/`priority()` 查询（崩溃报告/tracing 用）。
  - 未做（可选项）：`GetCurrentTaskImportance()` 继承当前线程重要性。
  - 验证：WSL 596 / Windows 636 全量 PASSED，post_job_bench 功能正常。

- **SingleThreadTaskRunner 增加 SHARED 模式（对齐 Chromium）** ✅ 已完成 2026-08-10
  （`SingleThreadTaskRunnerThreadMode::SHARED`，详见「最近完成」；2026-08-14 另修其 SHARED 死锁 `ff5aa06`）。
  可选遗留：`CreateSequencedTaskRunnerForResource(path)` 按资源缓存 runner（Chromium 唯一按 key 缓存 runner 的 API）。

- **TCPServerSocket_FDExhaustion** (POSIX, P2) ⏸️ 推迟:
  场景难以可靠构造（需 mock/epoll-free pump 或预配置 fd），且生产影响极低。

### P3

- **post_job Bench1 WSL 慢 9×（已深挖，2026-08-09）** 📐:
  已定位：**非 joiner 空转**（join 仅 0.8µs），真正根因是 **WSL2 单次线程 handoff（投递→唤醒→执行→完成信号）延迟 14.7µs vs Windows 6.5µs（2.3×）**。PostJob 每 job 需 2 次 handoff（main→worker + worker→main）→ WSL ~30µs/job vs Win 3.8µs。这是平台物理特性（futex/调度延迟），非库缺陷；Bench1 是每 job 1 op 的病态模式才暴露它。真实用户（job 有实质工作）不受影响；task_threadpool 连续投递 5.44M/s 证明机制高效。
  已关闭，无修复计划（除非未来引入"极小 job 由 joiner 直接完成、延迟投递 worker"的优化——Chromium 亦无此设计）。

- **`IoThread` dtor: WeakPtrFactory::InternalFlag 残留** ✅ 已调查关闭（2026-08-09）:
  ASAN 报告的 InternalFlag 间接泄漏已定位：`WeakPtrFactory<SequencedTaskQueue>`（195 个）+
  `WeakPtrFactory<TCPClientSocket::Impl>`。**决定性验证**：`ThreadPoolTest.RepeatedShutdownAfterWorkerIdleWaitNeverHangs`
  （150 次反复创建/销毁 ThreadPool）在 WSL ASAN 下 **exit=0 零泄漏** → **库核心无运行期累积泄漏**。
  此前 IO 测试的 4MB 全为 `Indirect`（reachable）残留 = 测试进程退出时的 reachable 对象
  （IOBuffer 全局池、静态持有者等），非 bug、无 OOM/性能影响（进程退出 OS 回收）。
  如需 ASAN 全量零报告需测试基础设施清理或 `detect_leaks=0`（Windows ASAN 已如此）。

- **Crash handler**: POSIX 信号处理器中非 async-signal-safe（已接受限制）。

- **TaskQueueSelector false sharing** 📐 2026-08-08:

  投递线程每次 `PostTask` 写入 `work_mask` 和 `active_priority_mask_`（atomic），
  selector 线程 `SelectNextQueue()` 读取这些字段同时写入 `schedule_counter_`。当前
  布局下这 3 个字段共享同一 cache line（offset 99-104），跨线程读写导致 cache 弹跳。

  **实测**（i5-10400T, fast-path OFF, 1M tasks）：OFF 比 ON 慢 ~8-15%（ON 旁路了 selector）。
  简单 `alignas(64)` 隔离 `active_priority_mask_` 反致倒退（-14.6%），原因是
  `SequenceManager::Impl` 内存布局整体受影响，`SelectNextQueue` 跨 cache line 数从 2→3。

  **方向**：需要对 `SequenceManager::Impl` 做整体 cache-line-aware 布局（热字段分
  group、冷字段单独隔离），而非单点修补。低优先级（当前 fast-path 默认 ON，
  selector 在 99% 场景不参与）。

- **PipeStream direct dispatch continuation**: batch-quota-exhausted 路径可直连
  而非经 `PostTask`。低优先级 — 当前设计正确。

### P3 — CMake `COMPILER_IS_*` 检测审查遗留（2026-08-11, `eb7431d`）⏸️ 推迟，手动处理:

- 🔴 clang-cl 归类缺陷、AppleClang WARNING 噪音
- 🟡 死代码、单语言检测、tests 漏改、mbedtls 行为变化
- ⚪ GNU 前端 else 分支过宽

由我手动清理，不纳入自动化任务。

---

## Known Flaky Tests 🔧 2026-08-10 (大部分已修复)

时序/环境敏感导致的偶发失败，非逻辑 bug（TaskRunner 层级重构后的 8 象限验证分诊）。

### WSL (Linux GCC)

| Test | Frequency | Root Cause | Status |
|------|-----------|------------|:---:|
| `PipeStreamTest.PosixYieldQuotaPreventsStarvation` | ~30% | 竞态：marker 在 read-done signal 前排队 | ✅ 已修复 |
| `ChildProcessTest.LaunchWithStdinPipeEchoesToStdout` | ~25-33% 间歇 | pull-read on_chunk 捕获栈事件引用——teardown 后迟到数据回调触碰已析构栈对象（UAF） | ✅ 已修复（`line_done` 改 shared_ptr + saw_line 守卫） |
| `TcpSocketTest.ServerDoesNotCrashUnderFdPressure` | 稳定失败 (WSL 9p) | 断言等 client connect 完成而非 server accept 处理完（实测 3/3 复现，accepted=0） | ✅ 已修复（accept settle 等待） |
| `ChildProcessTest.LaunchMultipleProcessesWithSharedProcessService` | 全量套件内稳定失败（standalone 通过） | 测试期望 bug：`IsRunning()` 反映全局 IOThread 单例，前序 ProcessService 测试已启动它 → 开头 `EXPECT_FALSE` 不成立 | ✅ 已修复（测试开头 `IOThread::ResetForTesting()`） |
| `HostResolverTest.*` (7 tests) | Always (WSL) | WSL 无 IPv6 / 外网 DNS | ✅ 已跳过 |
| `ThreadPoolTest.TaskObserverReceivesCallbacksWithPostedFrom` | 全量 ~1/400 偶发（复现实验 424/3000、63/3000 负载） | 丢失唤醒：`NotifyWorkAvailable` 未持 `wait_lock_` 即 Signal——通知落在 worker 代数检查与 `wait_cv_.Wait()` 之间时无接收者（cv 通知无记忆），worker 永久睡眠 | ✅ 已修复（Signal/Broadcast 改持 `wait_lock_`，与 `Shutdown` 同模式） |

### Windows (MSVC)

| Test | Frequency | Root Cause | Status |
|------|-----------|------------|:---:|
| `HostResolverTest.ResolveDualStack` | Always (no DNS) | 需外网 DNS | ✅ 已跳过 |
| `HostResolverTest.ResolveIPv4Only` | Always (no DNS) | 需外网 DNS | ✅ 已跳过 |
| `HttpStressFixture.ServerDestroyDuringTraffic` | e04 原代码 ~80% | server 销毁窗口内连接被 Orphan → drain-read 互等死锁（Windows 回环/IOCP 特有） | ✅ 已修复（`3cd8036`，18/18 通过） |

### TSan 专项（RelWithDebInfo）

| Test | 状态 | Root Cause | Status |
|------|------|-----------|:---:|
| `LogCTest.ConcurrentFirstUseInitializationStress` | TSan 失败 | TSan 慢速时序 | ✅ 已跳过 |
| `TlsSocketTest.LargePayloadBioCompaction` | TSan 失败 | TSan 慢速时序 | ✅ 已跳过 |
| `ThreadPoolTest.DelayedTaskRunsWithoutImmediateKick` | 全量 ~1/18 偶发 | auto-reset wake_event 丢唤醒 | ✅ 已加固 |

### Notes

- DNS 相关测试在具备完整网络访问的 CI 环境通过。
- Pipe/ChildProcess 偶发失败可考虑 `EXPECT_DEATH`-style 重试或事件同步修复。
- 低优先级 — 无崩溃/数据损坏/生产影响。

---

## 架构规划（memory 记录，未实施）

- **IOContext 重构 / IOThread 单例** ✅ 完成 2026-08-10（方向 C，全部 4 步完成）:
  - ✅ ① 新增 `IOThread::Get()` / `GetGlobalIOTaskRunner()` 单例 + AtExit
  - ✅ ② bench/测试/示例/`ProcessService` 改用共享 runner（10 文件迁移）
  - ✅ ③ 清理 pump 线程绑定语义（文档化惰性绑定、Current()、FdWatchController 生命周期）
  - ✅ ④ 双平台 IO bench 回归（async_file/pipe/tcp/tls 全部正常，无性能退化）
- **ThreadPool Pimpl 重构** ✅ 已实现（2026-08-09 核实 + `c1bb7db` 增量）：
  - roadmap 目标已全部落地：Pimpl 单例（`ThreadPool::Impl` + `ThreadPoolInstance`：Get/CreateAndStart/Shutdown/ResetForTesting + AtExit）、`CreateSequenced/SingleThread/ParallelTaskRunner` 工厂、queue 级保序（`Task.sequence_token` 经 runner FIFO 保证，token 传给 TaskObserver）、PooledTaskSource 注入式调度。
  - 本次增量：**ExecutionFence**（`ThreadPoolInstance::BeginFence/EndFence`，`c1bb7db`）——Chromium 对齐，fenced 时暂停派发新任务（运行中的完成），可嵌套，Shutdown 不挂起。3 个测试。限制：dedicated SingleThreadTaskRunner 不受 fence。
  - 可选后续：`GetCurrentTaskImportance` 继承（P2 已记录）、sequence_token 用于显式跨 runner 保序（当前无需）。

- **HttpServer 线程亲和与生命周期分析（2026-08-12 → 2026-08-13 全部处理 ✅）** 📐（`modules/neixx/net/src/http/http_server.cpp`）:
  **原分析结论**：可任意线程创建，但接口非线程无关 —— 建议「单线程所有权」模型。
  - ✅ **构造**：任意线程（`HttpServer::HttpServer()` 仅分配 `Impl`，无线程亲和）。
  - ✅ **AddRoute/AddWebSocketRoute data race** —— **已修复**（`f1aee94`）: `SharedState` 新增 `routes_mutex`，所有 Add*/Find*/Dispatch 持锁，handler **锁内复制、锁外调用**（符合"锁外回调派发"红线）。
  - ⚠️ **Listen 线程亲和** —— 设计保持并文档化: reactor 单线程模型为有意设计，头文件 Thread safety 段已完整声明，所有 handler/frame 回调在 io_runner 线程。
  - ✅ **Shutdown/析构** —— **已修复**（任意线程可调）: `accepting` 原子停收 → 锁下快照连接为 `vector<scoped_refptr<Connection>>`（强引用防悬挂）→ 逐个 `Close()`（跨线程自动 PostTask 到 io_runner 串行化）。
  - ✅ **UAF（Connection 裸持 `HttpServer*`）** —— **已修复**（`f1aee94`）: `Connection` 改持 `scoped_refptr<SharedState>`（RefCountedThreadSafe），accept 回调捕获 `[shared, runner]`；路由数据与服务器对象**解耦**，HttpServer 销毁后连接仍可安全 `Dispatch()`。
  **实现增强（对应原建议①②③，且更强）**:
  - ① 加锁 → `routes_mutex` 已实现
  - ② 连接跟踪 → `SharedState::connections` 注册表**持强引用**（显式 `AddRef`/`Release`，`Release` 在锁外防重入 `conn_mutex`），修复 **TSan 确认的 heap-use-after-free**（Shutdown 快照 vs 连接销毁竞态）
  - ③ 引用计数 → `SharedState` 分离（比 `scoped_refptr<HttpServer>` 更彻底）
  **额外强化**: Write queue（AsyncOutputStream 单在途写 + close-after-write flush）、线程安全 `Close()`、`accepting` 锁下二次检查（关闭 accept/Shutdown 注册竞态）。
  **验证**: `041293f` 多线程 HTTP/WS 压力测试。

---

## Future Feature Directions — 未排期（2026-08-12 评估）

| # | Feature | Rationale | 备注 |
|---|---------|-----------|------|
| 1 | HTTP/1.1 + WebSocket | ✅ **已完成 Phase 1**（2026-08-12） | Server/Client (TCP+TLS)、Parser (llhttp)、WebSocket 全栈、连接池、流式请求/响应、路由模式匹配 |
| 2 | HTTP/2 | ✅ **Phase A+B+C+D 全部落地**（2026-08-15）；**统一 HttpServer 单端口 ALPN 分流落地**（2026-08-15） | nghttp2 v1.70 已集成；Http2ClientSession 多路复用 + **统一 `HttpServer`**（h1+h2 单端口并存，Http2Server/旧 HttpServer 已并入；`HttpServerMuxTest` 5 用例覆盖共存/无 ALPN 兜底/流式双协议/Shutdown 排空）双平台全量过，TSan 10/10、valgrind 零错误；`http2_throughput_bench` 对照 H1（并发 8 流 1.85–3.16×） |
| 3 | SSL/TLS | ✅ mbedTLS 已集成 | SSLContext + TLS Server/Client Socket + ALPN |
| 4 | Storage Device Monitoring | 无 Chromium 参考，需自研 | Win RegisterDeviceNotification / Linux libudev |

---

## 最近完成（记录，2026-07 ~ 08）

- **ipc 模块专测 + 3 类库 bug 修复** ✅ 2026-08-14 — 新增 `tests/ipc_test.cpp`（11 用例：MessageChannel 8 + RpcEndpoint 3，
  内存双工流 LoopbackDuplexStream 驱动，覆盖往返/保序/分片/合帧/坏 magic/超长帧/优雅关闭排水/错误态丢弃/RPC 往返/超时/错误 abort）。
  测试揭露并修复的库 bug：
  - **死锁**：`MessageChannel::OnDataReceived` Phase 3 持 `lock_` 调 `BeginRead()`（内部再上锁，std::mutex 不可重入）——每次消息交付后 io 线程必卡死。修复：锁内决策、锁外调用。
  - **跨线程 WeakPtr FATAL（TSan/调试构建必炸）**：`WeakPtrFactory<Impl>` 绑定构造线程，但 WeakPtr 在 io/client 线程解引用（TSan 交叉线程检查直接 FATAL；Release 无检查掩盖风险）。修复：`Impl` 改 `RefCountedThreadSafe`，所有跨线程任务/IO 回调改 `WrapRefCounted(this)` 自持（库内 TCPClientSocket 同款惯例），shell 持裸指针 + AddRef/Release，析构即优雅 Close。TSan 验证 11/11 无 data race。
  - **帧尺寸错误 ×3**：`MessageChannel::Send`/`TryParseFrames`、`RpcEndpoint::BuildRpcFrame`/`ParseRpcFrame` 用 `IOBufferPool::AcquireBuffer`（桶规格化 size() ≠ 请求长度）→ 帧尾携带池内垃圾字节、交付消息 size() 错误。修复：改精确尺寸 `new IOBufferWithSize(n)`。
  - **测试侧 UAF**：两个 ChildProcess pipe 测试 on_chunk 捕获栈事件引用（teardown 后迟到回调 UAF），改 shared_ptr + saw_line 守卫；TCP fd 压力测试 accept 滞后断言改 settle 等待。
  验证：ipc 11/11 双平台过；WSL 全量 828/823；Windows 全量 842/841（1 偶发 = LaunchMultipleProcessesWithSharedProcessService，见 Known Flaky）。

- **SingleThreadTaskRunner SHARED 模式** ✅ 2026-08-10 — 新增 `SingleThreadTaskRunnerThreadMode::SHARED`。
  按 shutdown_behavior 分组共享 worker 线程；SharedWorkerThread 轮询处理多个队列。
  API: `CreateSingleThreadTaskRunner(traits, mode)`。4 个单测（同键同线程、不同键不同线程、DEDICATED 不同线程、大量 runner 不超限）。
  验证：Win 47/47 + WSL 47/47 ThreadPool 测试全通过。

- **退出阶段偶发挂起（遗留问题①）根因定位 + 修复** 🐛✅ 2026-08-09 —
  **症状**：Release 全量 GTest 退出阶段偶发挂起，595 PASSED 后进程不退出（主线程 `poll(eventfd)`），概率 <1/18。
  **根因**：`PooledTaskSource::Shutdown()` 设置 `is_shutdown_` + `wake_generation_.fetch_add` + `wait_cv_.Broadcast()` **均未持 `wait_lock_`**。worker 在 `GetNextTaskSourceTimed`/`WaitForDedicatedWork` 中持 `wait_lock_` 检查条件后进入 `wait_cv_.Wait()`；Shutdown 在 worker 尚未进入 futex 等待时 Broadcast 是 no-op → **condvar 丢失唤醒** → worker 永久睡眠 → 不 Signal `exit_event_` → `ThreadPool::Shutdown → JoinAll` 永久阻塞（主线程在 `TryJoin` 的 `exit_event_.TimedWait` = poll eventfd）。这是**逻辑时序竞态**（非 data race），TSan 检测不到，故此前 TSan 清零仍漏检。
  **修复**：`Shutdown()` 改为持 `wait_lock_` 设置 `is_shutdown_` + `wake_generation_` + `Broadcast`（锁内设置状态 + 唤醒，消除丢失唤醒窗口）。
  **验证**：新增压力测试 `ThreadPoolTest.RepeatedShutdownAfterWorkerIdleWaitNeverHangs`（150 次反复创建-销毁）；Windows Debug 85 task 测试全过；WSL Release 全量 596 PASSED + **8/8 轮全量退出无挂起**。
- **post_job_bench 跨平台崩溃修复（ffd3642）** 2026-08-09 — 根因链：① ShouldYield 用 running_workers_（含 work-stealing joiner）判断收缩 → pool worker 误判 over-subscribed → spawn-exit 风暴；② joiner 空转 → bench `nw->fetch_add` int 溢出 INT_MIN → `id>=w` 保护失效 → OOB。修复：ShouldYield 改用 assigned_workers_ + joiner 每 8 迭代让步 + bench id 改 uint32_t。Linux/Windows Release + ASAN 全平台验证不再崩溃。
- **post_job_bench 多 worker 偶发死锁修复（OnWorkerExited prev_assigned==1）** 🐛✅ 2026-08-09 — 症状：多 worker（w>=2）偶发卡死，卡在 Join 的 `completion_event_.Wait()`，所有 worker 已退出任务源循环（队列空、worker 空闲），但 `is_completed_` 从未设置。**根因**：`OnWorkerExited` 完成检测用 `prev_running==1`（running 含 joiner）判断"最后退出"+ 额外检查 `assigned<=0`——最后减 running 的线程可能观察到其他 worker 的 assigned 尚未 fetch_sub（竞态）而跳过完成信号，straggler 又因 `prev_running!=1` 不再复查 → 无人设置完成 → Join 死锁。**修复**：完成检测 key off `assigned_workers_`（`prev_assigned==1 && pending<=0`）——worker 是唯一减 assigned 的，最后退出必然 `prev_assigned==1`；joiner 不算 assigned 不干扰。**验证**：Windows 50/50 + WSL 30/30 post_job_bench 零死锁；双平台全量测试 task/PostJob 相关全过（HostResolver 5 个失败为环境外网 DNS 问题，与本次无关）。**重要教训**：Windows 构建必须**全量**（禁止 `--target X`），否则 `bench/tests` 目录 dll 不更新，exe 加载旧 dll 导致验证无效（曾致 2 小时无效调试；详见 memory `build-copy-dll-lesson`）。
- **Worker-Repost local-queue WSL 掉速修复（38e14a9）** 2026-08-09 — 根因：`DelayedTaskManager::OnQueueUpdated()` 对**纯立即 PostTask**（前后均无延迟任务）也无条件 `wake_event_.Signal()`，唤醒延迟任务线程查空堆又睡 → **WSL 调度乒乓**抢占投递 worker（rdtsc 实测 PostTask 偶发 11-154µs）。修复：仅当延迟状态实际变化（新增/清除/提前）才 Signal。效果：WSL repost 0.12→**0.32 M/s**（+160%），Windows 0.44→**0.56 M/s**（+27%）；差距 3.7×→1.75×。双平台全量无回归（WSL 596 / Win 636 PASSED）。教训：条件唤醒必须精确——无状态变化不 Signal，Linux/WSL futex wake+调度往返比 Windows event 贵。
- **TSan 竞态清零（2026-08-09）** — async_file 52 个（8f7d445 测试 shared_ptr<State> 范式）、net keepalive（c5f75a4 mutex_ 保护 keep_alive_dead_cb_）、log_runtime auto_flush_interval_ms（abaa069 原子化）。全量 TSan data races = 0。
- **Windows ASAN 全量通过（2026-08-09）** — 623 tests PASSED 0 错误（detect_leaks=0）。
- **Log 模块全面优化** 2026-07-31 — 死锁修复 + false-sharing 隔离 + timestamp 查表（+46%）。
- **SmallObjectAllocator + 独立 MemoryPressureMonitor** 2026-08-05 —
  PartitionAlloc 风格分配器（freelist 编码、decommit 中间态、多分区、committed/分桶统计）
  + Chromium 风格压力监控（平台策略 + listener + 有状态 monitor）；应用组装驱动回收。
- **ParallelTaskRunner Windows race（丢任务）** 2026-08-05 — **根因是 bench 假阳性**：
  并行投递时收到完成信号时可能仍有 in_flight 任务。sentinel 只保证出队顺序、不保证
  每个先前任务 body 已执行完毕；已由 `d928ba6` 的 `WaitForAllTasksExecuted()` 修复
  （等待 executed 计数达到预期，30s 停滞超时）。scheduler / callback 引用计数 /
  SmallObjectAllocator 均正确。
- **TimerTest.RepeatingTimerStopPreventsFurtherFires（SEH 0xc0000005）** 2026-08-06 —
  **根因是 Invoker 缺失 Chromium 的 WeakPtrCheck**：绑定 `WeakPtr` 的延迟任务在目标
  （timer）已析构后执行时，`std::invoke` 走 `*weak_ptr` 解引用，`get()` 返回 nullptr
  → `*nullptr` 崩溃。修复：`callback_internal.h` 的 `Invoker::Run` 调用前用
  `AllBoundArgsValid()`（fold 检查所有绑定 WeakPtr 有效性），任一失效即跳过整个回调
  （Chromium 语义）。已从 flaky 表移除。
- **PipeStream WSL 问题** 2026-07-25 — EPOLLONESHOT 显式 re-arm。
- **IOBuffer::data() 类型** 2026-07-29 — `char*`→`unsigned char*`（符号扩展 bug）。
- **FilePathWatcher** 2026-07-22 — inotify / ReadDirectoryChangesW。
- **TCPClientSocket::Impl Accept 路径泄漏** 2026-07-26 — Orphan() 加 `ReleaseSelfHoldIfNeeded()`。
- **OnceCallback 系列** 2026-07/08 — 模板化 + Chromium 风格 heap-only 重写（RefCountedThreadSafe + extern template ABI）。
