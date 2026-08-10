# libnei — TODO & Roadmap

**Updated**: 2026-08-10

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

- **PostJob 接口参数对齐 Chromium** ✅ 2026-08-09（`c09be98`）:
  `PostJob(from_here, traits, ...)` 此前被 `(void)` 丢弃、`CreateParallelTaskRunner(TaskTraits())`
  硬编码默认优先级。已修复：
  - ① `JobTaskSource` 构造接收 `Location`/`TaskTraits`，存 `posted_from_`/`traits_`；
  - ② `PostWorkers` 用 `PostTaskWithTraits(FROM_HERE, traits_, ...)` 透传 traits → worker 按
    traits 优先级执行（此前硬编码 USER_VISIBLE）；
  - ③ 提供 `posted_from()`/`traits()`/`priority()` 查询（崩溃报告/tracing 用）。
  - 未做（可选项）：`GetCurrentTaskImportance()` 继承当前线程重要性。
  - 验证：WSL 596 / Windows 636 全量 PASSED，post_job_bench 功能正常。

- **SingleThreadTaskRunner 增加 SHARED 模式（对齐 Chromium）** ✅ 已实现 2026-08-10:
  **现状**：`ThreadPool::CreateSingleThreadTaskRunner` 每次调用都新建独立 `PooledTaskQueue` +
  dedicated worker（计入 `max_num_workers` 上限）——runner/队列**不复用**（与 Chromium 一致：
  runner/Sequence 对象从不按 traits 去重）。
  **Chromium 差异**：`SingleThreadTaskRunnerThreadMode` 两档——
  `DEDICATED`（每 runner 独享新建 WorkerThread，runner 销毁时 `UnregisterWorkerThread` 归还）与
  `SHARED`（默认：`GetSharedWorkerThreadForTraits()` 按「环境 + shutdown 行为」从
  `shared_worker_threads_[env][continue_on_shutdown]` 复用共享 worker，惰性创建、`JoinForTesting()` 前不回收；
  多个 SHARED runner 共享同一线程但**各自独立 Sequence**——共享线程 ≠ 共享序列）。
  **待办**：给 `CreateSingleThreadTaskRunner` 增加 `SingleThreadTaskRunnerThreadMode` 参数并实现 SHARED
  模式，缓解大量 SingleThreadTaskRunner 撑爆 worker 上限；配套 `SameThreadUsed`/`DifferentThreadsUsed`
  风格单测。可选：`CreateSequencedTaskRunnerForResource(path)` 按资源缓存 runner（Chromium 唯一按 key
  缓存 runner 的 API，命中 CHECK traits 一致）。

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

---

## Known Flaky Tests 🔧 2026-08-10 (大部分已修复)

时序/环境敏感导致的偶发失败，非逻辑 bug（TaskRunner 层级重构后的 8 象限验证分诊）。

### WSL (Linux GCC)

| Test | Frequency | Root Cause | Status |
|------|-----------|------------|:---:|
| `PipeStreamTest.PosixYieldQuotaPreventsStarvation` | ~30% | 竞态：marker 在 read-done signal 前排队 | ✅ 已修复 |
| `ChildProcessTest.LaunchWithStdinPipeEchoesToStdout` | ~10% (Release only) | Pty/pipe teardown race | ⚠️ 已知 |
| `HostResolverTest.*` (7 tests) | Always (WSL) | WSL 无 IPv6 / 外网 DNS | ✅ 已跳过 |

### Windows (MSVC)

| Test | Frequency | Root Cause | Status |
|------|-----------|------------|:---:|
| `HostResolverTest.ResolveDualStack` | Always (no DNS) | 需外网 DNS | ✅ 已跳过 |
| `HostResolverTest.ResolveIPv4Only` | Always (no DNS) | 需外网 DNS | ✅ 已跳过 |

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

---

## Future Feature Directions — 未排期（2026-07-22 评估）

| # | Feature | Rationale | 备注 |
|---|---------|-----------|------|
| 1 | HTTP/WebSocket Server | 构建在 TCP 模块上 | Phase1 HTTP/1.1 → WS → HTTP/2 |
| 2 | SSL/TLS | 重依赖，HTTP/2 需要 | OpenSSL/BoringSSL/mbedTLS |
| 3 | Storage Device Monitoring | 无 Chromium 参考，需自研 | Win RegisterDeviceNotification / Linux libudev |

---

## 最近完成（记录，2026-07 ~ 08）

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
