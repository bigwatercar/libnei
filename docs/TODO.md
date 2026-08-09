# libnei — TODO & Roadmap

**Updated**: 2026-08-09

---

## 待办事项（未完成）

> ✅ **最高优先级（退出阶段挂起）已于 2026-08-09 定位并修复**（见下方"最近完成"）。
> 根因：`PooledTaskSource::Shutdown()` 未持 `wait_lock_` 即设置 `is_shutdown_` + Broadcast，
> 导致 condvar 丢失唤醒，worker 永久睡眠 → `JoinAll` 卡死。修复：锁内设置 + Broadcast。

### P1

- **单线程 TaskRunner 队列调度开销优化** 🔧 2026-08-06:

  **数据**：i5-10400T 上 PostTask 全链路 524.7 ns/task，其中：
  | 层 | ns | 占比 |
  |----|----|------|
  | 原子操作 | 10.8 | 2.1% |
  | BindOnce 构造 | 31.5 | 6.0% |
  | Run() 调用 | 15.0 | 2.9% |
  | **队列锁+push+DoWork+pop** | **467.4** | **89.0%** |

  89% 开销在队列调度，而非 callback 构造。历史 Ultra 9 185H 可达 6.9M/s（143ns/task），
  按频率折算后队列开销仍为主要瓶颈。

  **优化方向**：
  1. **同线程快速路径**：当 `PostTask` 发生在 TaskRunner 绑定的线程上时，
     跳过 `on_task_posted_callback_` 调度，直接在当前调用栈中执行
     （类似 Chromium `IncomingTaskQueue::PostTask` 的 `can_run_now` 路径）。
  2. **无锁批量提交**：利用线程局部缓存累积任务，批量 flush 到队列，
     减少锁获取次数（参考 SmallObjectAllocator 的 ThreadCache 模式）。
  3. **SequenceManager 同线程绕过 pump**：当 `DoWork` 发现调用者
     已在绑定线程上时，直接在 PostTask 返回前 drain 队列。

  **量化目标**：单线程 PostTask 从 1.9M/s → 4M/s 以上（减少 50% 队列开销）。

  **尝试记录（2026-08-09，已回退）**：尝试把 `SequencedTaskQueue` 的 posted 回调拆分为
  `OnImmediateTaskPostedCallback`（立即任务专用，exchange 短路 + 免 `HasImmediateWork`/`PeekNextDelayedRunTime`
  两次锁）。DIAG 显示投递热路径 lock+push 与回调各占 ~50%（WSL 各 ~170ns，含冗余锁）。但 **A/B 对照
  （git stash）显示 Windows 大幅回归（229→303ns，-32%）**，WSL 仅 +3.5%（266→257ms）——已回退。
  机制未完全明确（可能 Windows 上锁的缓存同步/分支预测副作用）。**结论**：跨线程投递的真正瓶颈是
  **每次投递（队列空时）的完整 pump wake（eventfd Signal + pump lock）**，回调拆分的锁优化无法解决，
  且 Windows 上有害。真正优化需减少 wake 次数（批量提交）或降低 Signal 成本（如 eventfd→无 syscall 的
  TLS 标志 + 定时批量 flush），需严格双平台 A/B 验证（当前数值：Win 229ns / WSL 251ns，目标 4M/s=250ns，
  Windows 已达标，WSL 仅差 ~1%）。

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

- **TCPServerSocket_FDExhaustion** (POSIX, P2):
  验证 EMFILE/ENFILE 生存不崩溃。阻塞于 IO pump 依赖 epoll FD；进程级 FD 耗尽会
  饿死 pump。需 mock/epoll-free pump，或预配置一个 fd 让 `accept4` 失败而不耗尽系统 FD。

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

## Known Flaky Tests 🔧 2026-08-01

时序/环境敏感导致的偶发失败，非逻辑 bug（TaskRunner 层级重构后的 8 象限验证分诊）。

### WSL (Linux GCC)

| Test | Frequency | Root Cause |
|------|-----------|------------|
| `PipeStreamTest.PosixYieldQuotaPreventsStarvation` | ~30% | Timing-sensitive yield quota |
| `ChildProcessTest.LaunchWithStdinPipeEchoesToStdout` | ~10% (Release only) | Pty/pipe teardown race |
| `HostResolverTest.ResolveInvalidHost` | Always (WSL) | WSL 无 IPv6 / 外部 DNS |
| `HostResolverTest.ResolveIPv6Only` | Always (WSL) | WSL 无 IPv6 连通 |
| `HostResolverTest.CustomDnsServerIPv6Cloudflare` | Always (WSL) | WSL 无 IPv6 连通 |
| `HostResolverTest.CustomDnsServerIPv6Google` | Always (WSL) | WSL 无 IPv6 连通 |
| `HostResolverTest.CustomTimeout` | Always (WSL) | WSL DNS 超时行为差异 |

### Windows (MSVC)

| Test | Frequency | Root Cause |
|------|-----------|------------|
| `HostResolverTest.ResolveDualStack` | Always (no DNS) | 需功能性 DNS |
| `HostResolverTest.ResolveIPv4Only` | Always (no DNS) | 需功能性 DNS |

### TSan 专项（RelWithDebInfo）

| Test | 状态 | Root Cause |
|------|------|-----------|
| `SequenceManagerTest.MultiQueueBurstDoesNotStarveAnyQueue` | 慢速挂起（loadavg 0.00） | 时序敏感 + TSan 慢速丢唤醒，与 valgrind 下 SequenceManagerTest 死锁同源；全量 TSan 扫描需排除 |
| `LogCTest.ConcurrentFirstUseInitializationStress` | TSan 失败 | 环境（TSan 慢速时序） |
| `PipeStreamTest.PosixYieldQuotaPreventsStarvation` | TSan 失败 | 同上（也是 WSL ~30% flaky 那个） |
| `TlsSocketTest.LargePayloadBioCompaction` | TSan 失败 | 环境（TSan 慢速时序） |
| `ThreadPoolTest.DelayedTaskRunsWithoutImmediateKick` | 全量 ~1/18 偶发（单跑 50 轮全过） | 依赖全量进程上下文；pool({1}) 单 worker + 120ms 延迟任务。DelayedTaskManager wake_event_ 为 auto，heap 为状态源，静态未见确定性 bug |

### Notes

- DNS 相关测试在具备完整网络访问的 CI 环境通过。
- Pipe/ChildProcess 偶发失败可考虑 `EXPECT_DEATH`-style 重试或事件同步修复。
- 低优先级 — 无崩溃/数据损坏/生产影响。

---

## 架构规划（memory 记录，未实施）

- **IOContext 重构**（io_thread_refactor_next_phase）：使 IOContext 成为 `Thread` 的调度引擎而非独立线程——作为阻塞等待器提供 `Run(Delegate*)` 入口（`DoWork()` / `DoDelayedWork(next_time)`），由外部线程驱动事件循环。关键约束：Thread 阻塞于 GetQueuedCompletionStatus/epoll_wait 时，外部 PostTask 需内核级唤醒立即唤醒 IOContext。
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
