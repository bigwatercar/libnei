# libnei — TODO & Roadmap

**Updated**: 2026-10-19

> 已完成的详细实现记录已归档至仓库记忆（`/memories/repo/`）与 `bench/results/`；
> 本文档只保留**未完成任务**与**里程碑摘要**，避免重复堆积。

---

## 未完成任务

### 触发式任务（有明确触发条件）

- **h2c 明文 HTTP/2 升级** — 触发条件：① 引入 gRPC 支持；② 明确内部明文 h2 多路复用需求。
  现状仅 TLS+ALPN h2；需服务端 `PRI * HTTP/2.0` 帧序言嗅探 + `Upgrade: h2c` 101 流程 +
  客户端 prior-knowledge/upgrade 双路径，ROI 低。

### StorageMonitor 剩余项（P1–P3 已交付，功能完整）

已交付内容见里程碑摘要（2026-10-14/15），这里只留**未完成**的验证与加固项：

- **真机验证缺口** — Windows Win7 回退路径（专用线程 + 隐藏顶层窗口 + `WM_DEVICECHANGE`）只有编译 +
  逻辑单测，无实机（Win8+ 的 `CM_Register_Notification` 路径已真机插拔验证）；Linux `device_id` 的
  `wwid:`/`serial:` 分级回退只由伪 sysfs 单测覆盖优先级逻辑，`/sys/block/<disk>/device/wwid` 与
  `device/serial` 属性名需在真实 Linux 机器插 USB 盘确认。
- **初次枚举在调用方序列同步执行** — 慢设备（休眠磁盘、机械盘）有阻塞调用方序列的风险；改成首次枚举也
  走 MayBlock 序列需要额外的“枚举完成前不派发事件”顺序保证，暂不动。
- **`Rescan()` 可能被在途旧扫描结果覆盖** — **已修（2026-10-18）**：每次 `Rescan()` 与每次异步
  扫描起手都会递增 `scan_generation_`，`OnAsyncScanDone` 发现代次不是最新就丢弃该结果（仅保留
  `scan_dirty_` 的后续重扫）。修复前该竞态会让旧快照回滚内部状态，并对观察者发出**反向**的
  ATTACHED/DETACHED 事件。回归测试 `StorageMonitorAsyncTest.StaleScanDoesNotOverwriteNewerRescan`
  （临时去掉代次判定即失败：`stale scan reverted the snapshot`）。
- **无线程池时事件重扫阻塞监视器序列** — 没有 `ThreadPoolInstance` 时事件触发的元数据扫描内联执行，
  慢设备会卡住监视器序列（当前接受）。
- **`Options` 里的测试钩子** — 现有 3 个：`on_event_rescan_for_testing`、
  `before_scan_completion_for_testing`、`after_async_scan_done_for_testing`（后两个为 2026-10-18 加，
  用于让"旧扫描结果 vs Rescan"的顺序可确定复现；它们必须在公共头里才能被测试跨 DLL 设置，
  因为内部符号未导出）。后续可用 friend 声明或内部头 + 导出测试工厂替换。

**有意不做**：Linux uevent（只报已挂载卷，挂载已由 mountinfo 覆盖，无收益）、网络盘（无场景，Windows
查询可能卡死）。

### 线程池既有语义缺口（2026-10-17 审查发现，均非近期修复引入）

对照 Chromium `base/task/thread_pool/thread_group_impl.cc` 审查 `e04c382` / `403ea22` / `581ced5`：三者都没有
破坏功能性语义。`suggested_reclaim_time` 的契约变更属实，但是**向 Chromium 对齐** —— Chromium 只在
`is_excess_`（超出 `initial_max_tasks`）的 worker 上按该值回收，非 excess worker 用
`GetSleepTimeout() = TimeDelta::Max()` 睡到无限，且它有 `EnsureEnoughWorkersLockRequired()` /
`MaintainAtLeastOneIdleWorkerLockRequired()` 按需重拉；本库没有该调度机制，故"初始 worker 永久存活"
是唯一不引入新调度器的正确解。

**2026-10-17 已修复（`fix(task): …` 提交）**：

- **补偿 worker 名额被已退出句柄永久占用** → 已修。`PoolWorkerBase` 统一持有 exit event 并在
  `ThreadMain()` 返回后 signal；`ThreadHandle` 增加 `try_join(timeout)`（0 = 非阻塞探测）；
  `ThreadGroup::ReapExitedWorkers()` 会 join 并移除已退出 worker，`SpawnCompensationWorker()` 在容量检查前
  先回收，`worker_count()` 也先回收再计数（语义改为"存活 worker 数"）。
  回归测试 `ThreadPoolTest.ExitedCompensationWorkersReleaseTheirCapacitySlots`：cap=2 的池连跑 4 轮
  "阻塞→补偿"，**移除回收逻辑时第 1–3 轮立即失败**（`no compensation worker was available`），修复后通过。
- **`Shutdown(timeout)` 的 timeout 未实现** → 已实现。`ThreadGroup::JoinAllWithTimeout()` 在预算内 join，
  超时则**保留句柄**（不 detach，可再次 join），返回 false；共享 worker 用同一 deadline 的 `TryJoin`；
  队列对象改为先进 `retired_queues_`、等所有 worker 都 join 后才释放（否则超时路径会释放 worker 仍在引用的
  队列）。头文件已写明：必须运行完的 BLOCK_SHUTDOWN 任务等待**不受 timeout 限制**，timeout 只约束最后的
  worker join 阶段。回归测试 `ThreadPoolTest.ShutdownWithTimeoutReportsWorkersStillRunning`。
- **`worker_count()` 语义** → 已修（见上），头文件同步更新。
- **SHARED 单线程 worker 在 `reclaim_time=0` 时轮询** → 已修：该路径改用 `wake_event.Wait()`（auto-reset
  event，投递任务与关闭都会 signal 且无等待者时保持置位，不会丢唤醒）。实测 WSL Release 空闲 3s 的 CPU 由
  **0.188s（6.3% 单核）降为 0.000s**；`reclaim_time` 为正时仍走 `TimedWait` 周期唤醒。
  回归测试 `ThreadPoolTest.SharedSingleThreadWorkerWithZeroReclaimTimeStaysResponsive`（覆盖语义；
  CPU 属性由 `%TEMP%\nei_shared_reclaim_probe.cpp` 探针测量，非单测断言）。
- **`RepeatingCallback::Run()` 空回调静默返回 `R()`** → **判定为非缺陷，仅补文档**。仓库已有测试把"运行
  已消费的 OnceCallback 是 no-op"固定为契约（`TaskCallbackTest.OnceCallbackRunsAtMostOnce` 对已消费回调
  第二次 `Run()` 并期望不执行），按"不改变既有语义"原则不加 DCHECK；改为在两个 `Run()` 上写明"空回调是
  no-op，用 `operator bool()` / `IsNull()` 判定"。零初始化修复已让这类判定可靠。
- **关闭期间新 spawn 的补偿 worker 不会被 join（潜在 UAF）** → 已修，三处：① `MaybeSpawnCompensationWorker()`
  在 `tracker_.HasShutdownStarted()` 时直接返回（关闭期不再扩容；运行中的任务进入 `ScopedBlockingCall`
  或 `MayBlock` 任务启动都会走这条路径）；② `~ThreadGroup()` 兜底 `JoinAll()`，绝不销毁仍在运行的句柄；
  ③ 关闭的 join 阶段改为 drain 循环（有界 8 轮），把在 join 期间漏进来的 worker 也 join 掉，再释放
  `retired_queues_`。新增测试专用访问器 `ThreadPool::worker_created_count_for_testing()`（累计创建数，
  `worker_count()` 看不出"创建后立刻退出"）。回归测试
  `ThreadPoolTest.NoCompensationWorkerIsSpawnedDuringShutdown`：**去掉①的检查后立即失败**
  （`created 2 vs 1`，"a compensation worker was spawned after shutdown took the worker set"），修复后通过；
  `ExitedCompensationWorkersReleaseTheirCapacitySlots` 也补了"累计创建数增长、存活数不变"的断言。

- **已投递但尚未开始执行的 BLOCK_SHUTDOWN 任务可能在关闭时被丢弃** → 已修（两处缺陷）。
  ① **语义缺失**：`Shutdown()` 原顺序是"取消非阻塞任务 → `CompleteShutdown()` → `task_source_.Shutdown()`"，
  还在队列里的 BLOCK_SHUTDOWN 任务只有恰好被 worker 先取走才会执行，否则随队列销毁被丢弃（Chromium 保证
  post→completion 全程阻塞关闭）。修法：新增 `PooledTaskQueue::GetPendingBlockShutdownTaskCount()`
  （`SequencedTaskQueue` 统计 `incoming_queue_` 里未交给 worker 的 BLOCK 任务；**不计**尚未到期的延迟任务，
  避免远期延迟任务无限期阻塞关闭——这是与 Chromium 的有意偏差），`Shutdown()` 在 `CompleteShutdown()` 之前
  以 1ms 轮询排空这些任务；`Shutdown(timeout)` 的预算同样约束该等待，到期则丢弃并打 warning；无超时时
  无限等待并在 5s 后打一次 warning 便于发现卡死。
  ② **计数簿记 bug（本轮排空等待暴露）**：`WorkerThread::ProcessTaskBatch` 的队列路径执行完任务会调用
  `tracker_->DidProcessTask(BLOCK_SHUTDOWN)`，但**从未调用对应的 `WillRunTask()`**，导致
  `num_items_blocking_shutdown_` 变负；`CompleteShutdown()` 的谓词是 `== 0`，于是永久挂起。此前因为
  `CompleteShutdown()` 通常在任务仍在运行时就被调用（计数为 0 直接返回）而侥幸不触发。修法：执行前调用
  `tracker_->WillRunTask()`（顺带让 SKIP_ON_SHUTDOWN 任务在关闭后正确跳过），空任务分支不再误减。
  回归测试 `ThreadPoolTest.ShutdownRunsQueuedBlockShutdownTask`：**移除排空等待后立即失败**
  （"a BLOCK_SHUTDOWN task that was still queued was dropped by shutdown"），修复后通过。
- **队列 → 任务源映射的数据竞态**（`fix(task): publish the owning task source on the queue instead
  of a locked map`）→ 已修。注册队列时在任意 shard 锁下写 `queue_to_source_`，而 worker 无锁读同一 map：
  除 TSan 数据竞态外，还可能让 worker 把刚注册的队列当未知队列而丢弃重入队（任务滞留）。修法：把
  `TaskSource*` 以 release/acquire 原子缓存在队列自身，顺带去掉 post 路径上的一次哈希查找。微基准单次
  开销 0.33ns(WSL)/1.99ns(Win)，原哈希查找 2.58ns/6.16ns，加锁 map 则需 6.42ns/17.22ns；端到端 10 轮
  Release 基准差异均在噪声内（±3.4%）。**压测证据**
  （`ThreadPoolTest.RunnerCreationWhilePoolIsBusyDoesNotLoseTasks`，4 线程并发建 runner 各投 100
  任务）：修复前 TSan 20 轮报 13 次竞态（读 `_M_find_before_node` ← `GetTaskSourceForQueue` ←
  `OnTaskQueueProcessed`，写 `_M_insert_bucket_begin` ← `RegisterTaskQueue` ←
  `CreateSequencedTaskRunnerLocked`）；修复前 WSL Release 1000 轮有 5 轮失败（`completed=300/400`，
  即某个 runner 的 100 个任务全部滞留）。修复后 WSL Debug/Release 各 1000 轮 + Windows Debug/Release
  各 200 轮全通过，TSan 50 轮 0 竞态。

**本轮验证矩阵**：WSL Debug/Release 全量、Windows Debug/Release 全量（各仅剩偶发
`PlatformThreadTest.SleepShortDurationPrecision`——单跑 3/3 通过，与本轮无关；两个 `PostJobTest`
隔离失败已于同日修复，见下方 `GetInstanceId` 条目）、WSL TSan（`ThreadPool*:ScopedBlockingCallTest.*:PostJob*:StorageMonitor*:TaskCallbackTest.*`
101 用例 0 竞态）、WSL ASAN（`build/linux-gcc-debug-asan`：线程池/存储/回调子集 99 用例 0 内存错误；
当时全量仅报与改动无关的既有泄漏——泄漏栈中 `thread_pool.cpp`/`thread_group.h` 出现次数为 0；
这批历史泄漏已于**同日全库清零**，见里程碑 2026-10-17）、
关闭相关新增与加固用例 WSL 三配置各 10 轮 + Windows 两配置各 20 轮全部通过。

**与 Chromium 的有意偏差（记录）**：① 尚未到期的**延迟** BLOCK_SHUTDOWN 任务不会阻塞关闭（Chromium 从投递
时刻就计数，远期延迟任务会一直等到其运行时间）；② 队列销毁时仍未执行的 BLOCK_SHUTDOWN 任务会随队列丢弃
（Chromium 视为不可达）。如需完全对齐，需要为延迟任务与所有丢弃路径补计数撤销，风险高于收益。

### 低优先（P3）

- **TaskQueueSelector false sharing 整体布局重构** — 方向：`SequenceManager::Impl` 做
  cache-line-aware 布局（单点 `alignas` 已实测反效 -14.6%）；fast-path 默认 ON，优先级低。
- **PipeStream direct dispatch continuation** — batch-quota-exhausted 路径可直连省一次 PostTask。
- **Crash handler POSIX 非 async-signal-safe** — 已接受限制（如需完整信号安全另行评估）。

### 推迟

- **TCPServerSocket_FDExhaustion**（POSIX, P2）— 场景难以可靠构造、生产影响极低。

### 可选 API 补齐（Chromium 对齐遗留）

- `GetCurrentTaskImportance()` 继承（PostJob 未做项）— 注：Chromium 该 API 现已重构为
  ThreadType（线程优先级）语义（`base/task/thread_type.h`），libnei 尚无线程优先级基础设施，
  若实现需先引入 ThreadType 或降级为独立 TaskImportance 枚举。
- sequence_token 显式跨 runner 保序（当前无需）。

---

## 里程碑摘要（已完成）

- **2026-10-19** — **TaskRunner 与 queue 生命周期（弱引用 → 引用计数）**。`SequencedTaskRunner::Impl`
  原来持 `WeakPtr<SequencedTaskQueue>`，而 `SequenceManager` 是线程栈对象；`WeakPtr::get()` 只读
  失效标记、不保命，poster 可读到“非空”后与 `~SequenceManagerImpl` 释放队列竞态（2026-10-18 在
  RelWithDebInfo+ASAN 下见过一次 `PeekNextDelayedRunTime` 读到已释放队列，之后 55 轮不复现）。
  修法：`SequencedTaskQueue` 改 `RefCountedThreadSafe`，manager 的 `queues_`/`shutdown_queues_` 与
  runner 都持 `scoped_refptr`（`CreateForSequenceManager()` 专门给 manager 用，避免 public
  `Create()` 重载让无关 TU 需要完整类型）；shut-down 后 post 由队列自身检查拒绝。
  回归用例：`ThreadTest.StaleTaskRunnerSurvivesSequenceManagerTeardown`（4 线程持续 PostTask
  与 `Thread` 销毁并发，之后 post 必须返回 false、不崩、不泄漏）。
  验证：Windows Debug 1065 / Windows Release 1059 / WSL Release 1058 / WSL rel-asan 1058（0 泄漏），
  TSan 目标 125 用例 0 race。

- **2026-10-19** — **HostResolver 测试的网络依赖 flaky 收口**（影响全量可信度）。`ResolveInvalidHost`
  依赖 NXDOMAIN 尽快返回，实际本机 DNS 转发对 `.invalid` 要 ~12 s，10 s 预算必超时；它超时后还会
  留下在飞查询，偶发带出 LSan 报“68583 B / 55 allocs 泄漏”。改为该用例单独给 30 s 预算（它只为
  “不崩不挂”兜底）。另外 `CustomDnsServerIPv6*` 原来的探针用系统解析器查 AAAA，证明不了
  “有 IPv6 路由”，于是无 IPv6 出口时会真跑并瞬间失败；改为用**测试实际使用的 IPv6 字面量 DNS 服务器**
  做探针（`HasIPv6DNSConnectivity(server)`），并为 `ResolveIPv6Only` 保留独立的
  `HasIPv6AddressResolution()`。结果：三平台全零失败，IPv6 用例正常 skip。

- **2026-10-18** — **`PlatformThreadTest.SleepShortDurationPrecision` 抖动误报消除 + 短睡眠改用高分辨率定时器**
  （Windows 全量原本每轮偶发 1 次）。第一阶段：该用例对 5 次 `Sleep(1ms)` 取**平均值**与 8 ms
  比较，单次被抢占的样本就能把均值拉过界（满载复现 `Average Sleep(1ms) took 14386 us` / `10492 us`，
  而每次 sleep 本身正常）；改为 10 次取**最小值**（抢占只会变长、不会让最小值变小），并把
  `high_resolution_clock` 换成单调的 `steady_clock`。
  第二阶段：全量下仍偶发失败，实测 `Fastest Sleep(1ms) took 14784 us` —— **最小值也接近一个 tick**，
  说明问题不在采样而是**全局定时器分辨率在长跑中被系统调粗**（`timeBeginPeriod(1)` 只作用于全局，
  且电源管理下可被回收）。根因修法：Windows 下 <10 ms 的睡眠改用**每线程高分辨率可等待定时器**
  （`CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`，Win10 1803+；
  老系统回退普通定时器、再回退 `Sleep+自旋`），不再依赖全局分辨率。
  验证：Windows Debug 全量 **1064 通过 ×2**、Release 全量 **1058 通过**；此前的重负载 200 次重复仍保持干净。

- **2026-10-18** — **ChildProcess 流代理的 IO hop UAF（真实库缺陷）**。现象：WSL Release 下
  `ChildProcessTest`/`TcpSocketTest`/`SequenceManagerTest` 组合跑约 2/12 段错误，且崩在**别的**
  用例执行期间。用 Release 时序 + ASAN（`RelWithDebInfo` + `-fsanitize=address`）复现到精确报告：
  `child_process_stream_proxy.cpp:122` 的 Step-2 hop 对**已释放**的目标流调用 `ReadAsync`
  （heap-use-after-free；释放侧是 `ShutdownOnIoThread()` → `core_.reset()` →
  `~PosixChildProcessCore` → `Cleanup()` 销毁 `stdout_stream_`）。
  根因：`ReadAsync()` 在**调用时**快照目标指针，而 hop 任务真正执行时目标可能已经没了 ——
  读完成回调会在 IO 线程上重新发起读（此时 `closed_` 仍为 false），而宿主线程同时析构
  `ChildProcess` 并投递 `ShutdownOnIoThread`，两个 `PostTask` 竞争同一队列的顺序，一旦
  shutdown 先入队，hop 就会在流销毁后执行。修法：把 target/closed 装进**引用计数的
  `InputStreamBinding`/`OutputStreamBinding` 令牌**（`std::shared_ptr`，hop 按值持有，
  不再需要解引用可能已析构的代理对象），并在 Step-2（IO 线程）上重新校验
  `binding->closed` 与 `binding->target == raw_target`；`ResetBinding()` 在销毁真流**之前**
  把 target 置空，因此校验是确定性的。同样修正了写方向的 `AsyncOutputStreamProxy::WriteAsync`。
  验证：RelWithDebInfo+ASAN 定向组合 **55 轮 0 报错**（修复前 11 轮内必现）、WSL Release 定向组合
  **20 轮 0 退、0 超时**（修复前基线 12 轮内 2 次段错误 + 1 次超时）、TSan 定向 32 用例 0 竞态、
  ASAN 全量 2 轮 1060 通过、Windows Debug 1063 / Release 1057 通过（仅已知 PlatformThread flake）、
  WSL Release 全量 2 轮 1054 通过。
  另外搭出的 `~/libnei_build/wsl-rel-asan`（RelWithDebInfo + ASAN）已经证明是复现这类**时序相关**
  内存错误的必备工具：Debug+ASAN 因太慢无法复现，纯 Release 无诊断信息，只有“Release 时序 +
  ASAN 诊断”两者兼备才能一步拿到使用/释放双侧栈。

- **2026-10-18** — **WSL ASAN 偶发 `stack-use-after-return` 定位并修复**（长期无栈悬案）。
  取证方法：① 先用最小探针确认 GCC 13.3 的 ASan **默认不检测**该类别，必须
  `ASAN_OPTIONS=detect_stack_use_after_return=1`（此前 20 轮"干净"因此不含有效信息）；
  ② 常规顺序全量循环至第 20 轮复现，取到完整栈。根因**在测试不在库**：\n  `tests/net/tcp_socket_test.cpp` 里多个用例把 TestBody 栈变量按引用捕获进
  `Listen()` 的 accept 回调 / 递归读回调，而用例返回时回调仍可能被触发 —— 拿到的两例是
  `ServerDoesNotCrashUnderFdPressure`（`server_fatal`，`Close()` 另投的失败回调在栈帧销毁后写入）
  与 `WriteChainNoStackOverflow`（`server_read_count`，递归读循环在用例返回后仍读到数据）。
  `TearDown` 是在 `TestBody` 返回**之后**才运行，所以 TearDown 里 drain runner 保护不了
  TestBody 的栈局部。修法：① 新增 fixture helper
  `CloseServerOnIoThreadAndDrain()`（在 IO 线程 `Shutdown()` + 销毁 server，再在同一任务内投
  fence 并等待），所有注册 accept 回调的用例在返回前调用它；② 递归读/写循环这类**回调自身
  持有 state** 的用例（`ServerDoesNotCrashUnderFdPressure`、`WriteChainNoStackOverflow`）
  把跨线程状态搬进 heap `struct State` 按值捕获，并在收尾显式关闭已 accept 的 socket。
  **注意 helper 必须用 `Shutdown()` 而不是 `Close()`**：`Close()` 会以 `success=false` 触发
  accept 回调，反而打破这些用例已经做过的成功断言。
  验证：回退修复的定向 150 轮里 1 次复现（同一签名），修复后 150 轮 0 次；`TcpSocketTest.*`
  ASAN（UAR 检测开）40 轮 0 报错 0 失败；ASAN 全量（UAR 检测开 + LSan）**连续 5 轮
  1060 通过、0 报错、0 泄漏**（修复前约 1/10 轮命中 UAR）；Windows Release 全量仅已知
  `PlatformThread` flake，WSL Release 全量 1054 通过。
  另记录一条顺带发现的健壮性问题：`child_process_impl_common.h` 的 `Launch()`/`Terminate()`
  未检查 `PostTask` 返回值，service runner 已停时会永久阻塞在 `done.Wait()`（`Shutdown()`
  早就处理了这一点），已按同样语义补齐失败分支。

- **2026-10-17** — 全库 ASAN/LSan 泄漏清零（全量 1042 测试，0 泄漏）。其中 3 处是
  **真实库缺陷**：① `HttpClient::SendRedirecting` 的递归 hop driver 强引用自身，
  `run → lambda → state → run` 自环使每个重定向请求泄漏 state / `HttpClient::Impl` /
  最后一个响应（改为弱自引用 + 由在飞跳回调强持有 state）；② `TLSManager::FreeSlot`
  只清指针不跑析构，包装对象先于线程退出销毁时 `ThreadLocalOwnedPointer<T>` 的值被
  静默丢弃（改为释放线程先销毁自己的值）；③ c-ares 回调在 WeakPtr 丢弃路径漏掉
  `ares_freeaddrinfo`（改为 RAII 守卫）。其余为测试端生命周期问题：runner 停止后才
  销毁 `TCPServerSocket` / `AsyncFile`（`Orphan`/`DeleteSoon` 投递到已停线程）、测试
  回调自引用、UDP 压测 recv driver 的 state 环。

- **2026-10-14 / 15** — `StorageMonitor` 模块 P1–P3 落地（对齐 Chromium
  `components/storage_monitor/`：`StorageMonitor` + `StorageInfo` + `RemovableStorageObserver`，
  PIMPL + 回调经 runner 投递；共 22 测试、Win 侧重后端路径 8 例，TSan 干净）：P1 Linux
  `/proc/self/mountinfo` + `FdWatchController` 快照 diff（过滤 `9p`/`tmpfs`/`proc`/`overlay`，不依赖
  libudev）→ P2 Windows 双路径（Win8+ 运行时 `GetProcAddress` 探测 `CM_Register_Notification`；
  Win7 回退专用线程 + 隐藏**顶层**窗口 + `WM_DEVICECHANGE`，因卷广播不发给 message-only 窗口；
  真机 USB 插拔两条路径均收到 ATTACHED/DETACHED）→ P3 `EjectDevice`（Linux `umount2` / Windows
  锁卷+卸载+弹出，固定盘拒绝；真机验证 kOk/kInUse）+ 事件重扫改线程池 MayBlock 序列并合并。
  同期改进：`EjectDevice` 改为实时解析弹出目标（+2 测试），`device_id` 分级回退
  `uuid:` → `partuuid:` → `wwid:/serial:`（+分区号）→ `path:`（+1 测试）。
  **P3 真机复验顺带暴露并修复两个既有线程池缺陷**：初始 worker 空闲 30s 自行退出后无重拉起
  （`fix(task): keep initial thread pool workers alive when idle`；修复后空转 84s 再插拔仍正常）、
  队列 → 任务源映射的无锁竞态（`fix(task): publish the owning task source on the queue instead of a
  locked map`，详见线程池章节）。剩余验证与加固项见「StorageMonitor 剩余项」。

- **2026-09-03** — CMake 编译器检测重构：`COMPILER_IS_*` 内联检测逻辑从 top-level
  CMakeLists 提取为 `cmake/neiDetectCompiler.cmake` 模块（include-once 保护），统一为
  `NEI_COMPILER_IS_MSVC/GNU/APPLE_CLANG`、`NEI_MSVC_FLAVOR` / `NEI_GNU_FLAVOR` 等变量；
  top-level 与 external/（c-ares、nghttp2 等）消费者全部改用新变量；mbedtls 增加
  `CMAKE_C_COMPILER_FRONTEND_VARIANT` 兼容（clang-cl/MSVC）；新增 cmake 格式配置
  （`e4fe29d`）。
- **2026-08-29** — HTTP 中间件最小形态落地：`HttpServer::AddFilter` 全局前置过滤器
  （h1+h2 双协议，路由派发前按序执行，false 短路 + 默认 403，可注入请求头；8 测试，
  Win/WSL 全过）。TODO 唯一显式剩余项清除。
- **2026-08-28** — `native_library` 模块：NativeLibrary 裸句柄 + 自由函数 + ScopedNativeLibrary
  RAII（对齐 Chromium）；12 测试，Win 1030 / WSL 1008 全量过，ASAN/TSan 干净（`354a031`）。
- **2026-08-26** — `PostTaskAndReply/WithResult`；HttpFileTransfer 写盘背压。
- **2026-08-24** — HttpClient 流式下载背压（Resume 协议）；HTTP 代理（CONNECT 隧道 + TLS）。
- **2026-08-23** — gzip/deflate 压缩（vendored zlib）、Cookie（RFC 6265）、multipart（RFC 7578）、
  `SendRedirecting` 自动跟随（含 keep-alive endpoint 校验缺陷修复）。
- **2026-08-22** — HTTP/2 优先级 RFC 9218 修复（发现 nghttp2 `submit_priority` no-op）。
- **2026-08-18** — 唤醒路径回移植（`970e7b6`）：WSL dedicated 2.26→4.26M/s（+88%）；AtomicEvent 落地。
- **2026-08-15** — HTTP/2 Phase A–D 全落地；统一 HttpServer 单端口 ALPN 分流；
  spin-then-park 实验否决并回退。
- **2026-08-14** — CLOSE_WAIT 堆积修复（idle timeout + Peek 探活）；SHARED 死锁修复；
  ServerDestroyDuringTraffic 死锁修复（`3cd8036`）。
- **2026-08-13** — HTTP 大数据渐进式传输四阶段（SendStreaming / SendBody / HttpFileTransfer /
  服务端一致性）全落地。
- **2026-08-12** — HTTP/1.1 + WebSocket 全栈；HttpServer 线程亲和修复（SharedState 解耦）。
- **2026-08-10** — IOThread 单例（方向 C）；SingleThreadTaskRunner SHARED 模式。
- **2026-08-09** — ThreadPool Pimpl + ExecutionFence；PostJob 接口对齐 Chromium。
- **2026-08-08** — Parallel worker-repost 缺陷修复（`a01dd2a`）；SmallObjectAllocator 堆损坏根因。
- **2026-07** — TLS / UDP / SharedMemory / TCP C10K 攻坚 / Keep-Alive / OS 信息模块（详见 repo memory）。

## Known Flaky（均已于 2026-08 修复或跳过，详见 repo memory `testing-lessons`）

- 丢唤醒类：`TaskObserverReceivesCallbacksWithPostedFrom`、`DelayedTaskRunsWithoutImmediateKick`。
- UAF/竞态类：`ClientOrphanDrainReadEOF`（栈事件）、`ServerDoesNotCrashUnderFdPressure`、
  Pipe/ChildProcess 迟回调。其中 `ServerDoesNotCrashUnderFdPressure` 长期只是"偶发跳过"，
  **2026-10-18 已真正根因关闭**：accept 回调捕获 TestBody 栈 atomic，`Close()` 迟投的
  失败回调在栈帧销毁后写入（ASAN `stack-use-after-return`）；改为 heap state + IO 线程
  fence，详见里程碑 2026-10-18。
- 环境跳过：WSL 5 项 HostResolver DNS、Win 3 项 DNS/IPv6、TSan 2 项时序敏感。
- **ChildProcess 迟读回调段错误（2026-10-18 发现并修复，`11bcc35`）**：WSL Release 下用 filter
  `TcpSocketTest.*:SequenceManagerTest.*:ChildProcessTest.*` 循环跑 12 轮，未改动的基线就
  约 2/12 段错误、另有 ~1/12 次 `ChildProcessTest.LaunchWithStdinPipeEchoesToStdout`
  5 秒超时。gdb 回溯显示崩溃在 `IOThread` 的
  `AsyncInputStreamProxy::ReadAsync` lambda（`SequenceManager::Impl::DoWork` →
  `MessagePumpForIO::Run`），且崩溃时正在跑**下一个**用例 —— 即上一个 ChildProcess 用例
  退出后仍有在飞 stdout 读回调。单测单独跑 100 轮不复现（需要组合/前序状态）。
  → **已定位为库缺陷并修复**：IO hop 未在**执行时**重新校验目标流（`ShutdownOnIoThread`
  先 ResetBinding 再销毁真流，而 hop 与 shutdown 的 PostTask 顺序是竞争的），详见里程碑
  2026-10-18 的"ChildProcess 流代理的 IO hop UAF"条目；修复后定向 55 轮（RelWithDebInfo+ASAN）
  与 20 轮（Release）均干净。
- **间歇 LSan 引用环泄漏（2026-10-18 发现并修复，`b35afe4`）**：WSL ASAN 全量报
  4880 B / 13 allocations，全部为 indirect、无 direct（引用环特征）。涉及 `IOThread` 的
  `MessagePumpForIOState`/`SequenceManager`/`SequencedTaskQueue` 以及
  `PipeStreamTest` 里 `ReadAsync` 回调捕获自身 `shared_ptr`
  形成的自环。→ 根因：`RapidCancelAndRetryStateMachine` / `RapidWriteCancelAndRetry`
  的第二阶段让读回调捕获**自己所属的流**（`stream → callback → shared_ptr<stream>`），
  测试返回后该环还持有在飞读、注册的 fd watch，并**间接**拖住 IOThread 的
  Manager/pump。修法：流提升到测试作用域、回调不再自捕获，返回前在 IO 线程
  `Close()`+析构并 fence（新增 fixture helper `CloseStreamOnIoThreadAndDrain`）。
  验证：定向 `PipeStreamTest:IPCTest` ASAN 30 轮 0 泄漏（修复前 6 轮内命中）、
  ASAN 全量 3 轮 1060 通过 0 泄漏。
- **测试隔离（v0.9.1 即存在，2026-10-14 确认）**：`PostJobTest.RepeatedMultiWorkerJoinNeverHangs` /
  `MultiWorkerCompletesExactBudget` 在全量运行（及 IOThread+CurrentSupport+FilePathWatcher+
  ThreadPool*+PostJob 的组合）下稳定失败（`total_done` 为 0），单独运行通过。
  → **已修复（2026-10-17，`8db3ef6`）**：根因是 `PostJob` 的 8 个 `ParallelTaskRunner` 缓存在函数内
  静态变量里，只创建一次；`ThreadPoolInstance::Shutdown()`/`ResetForTesting()` 后缓存仍指向已退役的池，
  投递会被接受但队列永不排空 → `done == 0`。修法：缓存改按 `ThreadPoolInstance::GetInstanceId()`
  （每次 `CreateAndStart` 自增）失效重建；不能只比较指针，因为新实例可能落在同一地址（TSan 下实测
  两次打印同一 pool 指针）。回归测试 `PostJobPoolLifecycleTest.JobsRunAfterThePoolIsRecreated`
  （回退修复即报 `actual: 0 vs 100000`），修复后四配置全量均无 `PostJobTest` 失败。
