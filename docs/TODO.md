# libnei — TODO & Roadmap

**Updated**: 2026-08-06

---

## 待办事项（未完成）

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

- **Parallel worker-repost 缺陷：`running_worker_count_` 负溢出 + 任务重复执行 + 死锁** 🐛 2026-08-08:

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

- **PostJob 接口参数对齐 Chromium** 🔧 2026-07-30:
  `PostJob(from_here, traits, ...)` 的 `from_here` 和 `traits` 被 `(void)` 丢弃，
  `CreateParallelTaskRunner(TaskTraits())` 硬编码默认优先级，丢失调用方优先级控制
  和调用位置追踪。
  - **Chromium 参考**: `base/task/post_job.cc` — `from_here` 存入 `JobTaskSource`
    用于崩溃报告/tracing；`traits` 控制 worker 线程优先级与 ThreadPolicy；
    `GetCurrentTaskImportance()` 继承当前线程重要性。
  - **方案**: ① `JobTaskSource` 构造函数接收 `Location`/`TaskTraits`；
    ② `PostJob` 将 `traits` 透传给 `CreateParallelTaskRunner`；
    ③ `from_here` 存入 `JobTaskSource` 用于 `posted_from()` 查询。

- **TCPServerSocket_FDExhaustion** (POSIX, P2):
  验证 EMFILE/ENFILE 生存不崩溃。阻塞于 IO pump 依赖 epoll FD；进程级 FD 耗尽会
  饿死 pump。需 mock/epoll-free pump，或预配置一个 fd 让 `accept4` 失败而不耗尽系统 FD。

### P3

- **`IoThread` dtor: WeakPtrFactory::InternalFlag 残留 (~1KB)** 🔧 2026-07-27:
  ASAN 报告 ~1KB（17 分配）间接泄漏 — 未释放的 `WeakPtr` 仍引用 InternalFlag。
  影响可忽略（固定 ~1KB，进程退出时由 OS 回收），无 OOM/性能风险。
  Next steps: 调查 `SequenceManager::Shutdown()`/`TaskQueue` 清理，确保
  `WeakPtrFactory` 失效前 drain 所有 outstanding `WeakPtr`。低优先级。

- **Crash handler**: POSIX 信号处理器中非 async-signal-safe（已接受限制）。

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

### Notes

- DNS 相关测试在具备完整网络访问的 CI 环境通过。
- Pipe/ChildProcess 偶发失败可考虑 `EXPECT_DEATH`-style 重试或事件同步修复。
- 低优先级 — 无崩溃/数据损坏/生产影响。

---

## Future Feature Directions — 未排期（2026-07-22 评估）

| # | Feature | Rationale | 备注 |
|---|---------|-----------|------|
| 1 | HTTP/WebSocket Server | 构建在 TCP 模块上 | Phase1 HTTP/1.1 → WS → HTTP/2 |
| 2 | SSL/TLS | 重依赖，HTTP/2 需要 | OpenSSL/BoringSSL/mbedTLS |
| 3 | Storage Device Monitoring | 无 Chromium 参考，需自研 | Win RegisterDeviceNotification / Linux libudev |

---

## 最近完成（记录，2026-07 ~ 08）

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
