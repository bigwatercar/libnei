# libnei — 项目背景与经验教训速查

> 本文档汇总项目背景、硬性规范与历次踩坑教训，用于切换开发环境后快速接续工作。
> 详细路线图与待办见 `docs/TODO.md`。实现决策以 Chromium 上游为准，本文档仅作参考。
> 最后更新：2026-10-18。

---

## 1. 项目概览

- **定位**：仿 Chromium `base` 库设计哲学的底层 C++ 基础设施库（C 库 `nei` + C++ 库 `neixx`）。
- **标准**：C++17（禁用 C++20 特性）、C 模块用 C99。
- **平台**：Windows（MSVC/VS2022）+ POSIX（GCC，Linux/WSL）。
- **命名空间**：`nei::`；文件后缀 `.h` / `.cpp`。
- **模块布局**：`src/nei`（C 库）→ `src/neixx`（C++ 库，链接 nei）；模块目录规范
  `include/neixx/<module>/<public_api>.h` + `src/<impl>.cpp`。
- **构建**：单一 `nei` target 输出一个 DLL/SO；CMake 的 `target_sources()` 必须显式列出 `.h`
  头文件；公共头经 PUBLIC include 路径暴露。CMake 选项/宏统一 `NEI_` 前缀。
- **库模式**：同时支持静态与动态库（`NEI_API` 导出宏）。

### 构建与验证矩阵

| 构建目录 | 用途 |
|---|---|
| `build/windows-vs2022-shared` | Windows Debug/Release 主验证（全量构建） |
| `build/windows-vs2022-asan` | Windows ASAN |
| `build/linux-gcc-release-shared` | WSL/Linux Release 主验证 |
| `build/linux-gcc-tsan` | WSL TSan |
| `build/linux-gcc-debug-asan` | WSL ASAN（Debug 时序） |
| `~/libnei_build/wsl-rel-asan` | WSL RelWithDebInfo + ASAN（Release 时序 + 诊断）——复现时序型内存错误的唯一有效配置，见下 |

- **Windows 构建硬红线**：禁止 `--target X` 局部构建——必须全量
  （`cmake --build build\windows-vs2022-shared --config <cfg>`），否则子目录
  （bench/tests）的 `nei.dll` 不会随 POST_BUILD 拷贝更新，验证跑在旧二进制上
  （曾造成约 2 小时无效调试）。运行前核对 exe 目录下 `nei.dll` 时间戳与根目录一致，
  必要时 `findstr /c:"新字符串" nei.dll` 确认新代码进二进制。
  WSL/Linux 无此问题（.so 实时指向 build 目录）。
- **测试**：Google Test，目标 `nei_tests`。当前全量基线（2026-10-18，全部零失败）：
  Windows Debug **1064** / Windows Release **1058** / WSL Release **1054** / WSL ASAN **1060**
  （另 5 项环境跳过：`NativeLibraryTest.LoadSystemLibraryKernel32` 与 `HostResolverTest` IPv6 系列）。
- **消毒器**：Windows ASAN 全量 912+/0 报告；WSL TSan 全量（排除下述环境项）0 竞态；
  valgrind 用于 UAF/OOB（**禁止**用于时序敏感的并发测试——会死锁，需 `timeout` 包裹）。
- **ASan 的 UAR 检测默认是关闭的**（2026-10-18 教训）：必须显式
  `ASAN_OPTIONS=detect_stack_use_after_return=1`。此前多次「跑了很多轮都干净」实为关闭检测的
  空跑、零信息量。换环境/换编译器后先用最小探针（局部数组地址存到全局再读）自证检测真的会报
  `stack-use-after-return`，再开始计时取证。
- **Debug + ASAN 复现不了时序型内存错误**（2026-10-18 教训）：ChildProcess 流代理 UAF 在
  Debug+ASAN 下 20 轮不复现，纯 Release 会段错误但无诊断信息。正解是 RelWithDebInfo + ASAN：
  既保留 Release 时序，又有完整的使用/释放双侧栈。离线环境需
  `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` 并用 `-DFETCHCONTENT_SOURCE_DIR_*` 指向已有 `_deps/*-src`。
- **已知环境性失败**（与代码无关，全量需排除）：
  - WSL：`HostResolverTest` IPv6/DNS 系列（CustomDnsServerIPv6Cloudflare/Google 等）；
  - TSan 慢速挂起项：`SequenceManagerTest.MultiQueueBurstDoesNotStarveAnyQueue`。
- **WSL2 TSan 运行前提**：`sudo sysctl -w vm.mmap_rnd_bits=28` 且 `setarch $(uname -m) -R`。
- **Benchmark**：一律 Release 模式收集；任务调度 bench 默认 1M 任务；结果存
  `bench/results/`，基线 `baseline_(Ultra9-185H)_20260816_windows.md`。
- **clang-format**：每次提交前 `clang-format -i` 全部变更的 C/C++ 文件
  （`C:\opt\devutils\clang-format.exe`）；**严禁格式化 `external/`**（`mbedtls_config.h`
  是手动修改的例外）；格式化后必须双平台重构建验证再提交；格式化只在代码稳定后
  提交前做一次（中途格式化会让后续文本匹配编辑失效）。

---

## 2. 架构红线（不可违反）

1. **异步回调确定性**：派发给用户的回调必须唯一稳定地运行在绑定 runner 上；严禁在
   API 入口同步触发回调（包括失败路径），必须 Post 异步投递。
2. **锁外回调派发**：绝对禁止持锁调用外部业务回调。锁内摘除/移动状态 → 解锁 → 锁外派发。
3. **WeakPtrFactory 必须是类最后一个成员**：析构时先 Invalidate 所有 WeakPtr。
4. **头文件纯净度**：公开头严禁 `windows.h`/`unistd.h`/`pthread.h`/`std::mutex`/
   `std::atomic`/`HANDLE`/`int fd`；跨平台句柄统一 `PlatformHandle`；复杂状态用 PIMPL。
5. **平台拆分**：短分支同文件 `#if defined(_WIN32)`；长代码拆 `_win.cpp`/`_posix.cpp`
   配 `_win.h`/`_posix.h` 内部头（参照 AsyncFile/PipeStream/TCP 模式）。
6. **注释 ASCII-only**（避免 MSVC C4819）。
7. **接口语义即契约**：预发布期允许 breaking change，但不能悄悄改变现有 API 语义
   （如优雅 Orphan 改硬 Close）；语义变化必须新增显式 API（如 `Abort()`）由协议层选择。
8. **CHECK 宏**：`DCHECK_EQ_MSG(lhs, rhs, "msg")` 形式，不支持 `<<` 流式。

---

## 3. 生命周期/线程安全决策速查

### 跨线程回调保活：三选一

| 方案 | 适用 | 要点 |
|---|---|---|
| 普通 `WeakPtr` | 工厂与解引用同线程；"对象死后回调静默取消"正是想要的语义 | 只读 invalidation flag，**不保命** |
| `WeakPtrThreadSafe<T>` 特化 | 跨线程解引用但外部契约保证对象不早死 | 只关 debug 检查，Release 本就无检查——安全全靠外部契约；需一行注释向审阅者证明 |
| `RefCountedThreadSafe` + `WrapRefCounted(this)` | 回调必须送达、对象可任意线程消亡、无关闭顺序保证 | 库内 async I/O 对象标准惯例（TCP/TLS/UDP/IPC）；回调捕获强引用自持 |

判定三步：回调丢了没关系吗？→ WeakPtr；跨线程但有外部保证？→ 特化；否则 → RefCountedThreadSafe。

### 其他标准模式

- **锁边界范式**（`conn_mutex_`）：状态机内部直接访问成员，仅跨线程探针/写边界加锁，
  锁内复制/决策、锁外回调。
- **不可变发布 + 永不 free**：无锁读者缓存 vs 写者 slot 复用 → 用 malloc 不可变副本
  发布、旧副本永不回收。
- **IO 线程单所有权**：socket/fd/nghttp2 会话只由 I/O 线程触碰；跨线程操作 PostTask
  hop 到 io_runner 串行化。
- **写队列 + close-after-write flush**：`Close()` 延迟到在途写排空（否则对端收到 FIN
  先于最后响应字节）；flush-then-close 而非 shutdown+drain（drain 等对端 FIN 会互等挂死，
  见 TCPClientSocket Orphan 教训）。
- **测试 teardown fence**：销毁会触发异步回调的对象必须在回调所属线程销毁并在返回前
  排空；fence 事件从 I/O 线程任务**内部**投递（主线程投递会排在 Close 内部 Post 的
  回调之前）；测试回调捕获用 `auto state = std::make_shared<State>()` + `[state]`
  值捕获，绝不用 `[&]` 捕栈。
- **条件变量协议**：`Shutdown()`/通知必须持 `wait_lock_` 设置共享状态再 Broadcast
  （否则丢唤醒）；精确唤醒——无状态变化不 Signal（futex wake 在 WSL2 很贵）。
- **AtomicEvent 单字事件**（`neixx/synchronization/atomic_event.h`，2026-08-16 落地）：
  32 位原子 = bit0 SIGNALED + 高位 waiter 计数；Linux futex / Windows WaitOnAddress；
  Signal 无等待者时零 syscall。**实现要点**：Wait 的 Park 期望值必须 `& ~kSignaledBit`
  （否则令牌停在 add 与 park 之间会睡死）；Signal 幂等（令牌已停即 no-op）→ 需要
  "每消费一次唤醒一次"的握手测试必须逐轮等待确认。**Windows 版本约束**：`WaitOnAddress`
  是 Win8+ API——必须 GetProcAddress 动态解析（静态引用会让整个 nei.dll 在 Win7 加载
  失败 0xC0000139）；Win7 降级到 `CRITICAL_SECTION + CONDITION_VARIABLE` fallback
  （Vista+），语义一致仅性能退化。bench：Win 319 / WSL 314 ns/轮，vs condvar 727/1019、
  WaitableEvent 1090/1144。

### 使用方契约：库本身没问题，错在用法（客户代码最容易踩的坑）

> 这一组缺陷**全部**发生在调用方（我们的测试就是第一个客户）：库的语义是对的，但因为误用
> 异步 API 而产生 UAF / 泄漏 / 挂死。客户代码请逐条对照自查。

| 误用形态 | 症状（消毒器怎么报） | 正确用法 |
|---|---|---|
| **回调里按引用捕栈变量**（`[&]` 捕 `WaitableEvent`、计数器、`std::string`） | ASan `stack-use-after-return`：写入方是 I/O 线程，而内存属主是调用线程已返回的栈帧 | 回调生命周期可能长于调用栈时，状态搬进 heap `struct`（`auto state = std::make_shared<State>()`）按值捕获，回调里只碰 `state->...` |
| **以为在 TearDown / 析构里 drain 就能保护栈局部** | 同上，且只在「函数返回后仍有在飞回调」时偶发（实测约 1/10 全量轮次） | `TearDown` 在 `TestBody` **返回之后**才跑，保护不了栈局部；必须由使用方**在返回前**排空：在回调所属 runner 上 Close + 析构，再从**该任务内部**投 fence 并等待 |
| **回调捕获自己所属对象的 `shared_ptr`**（`stream->ReadAsync(..., [stream]{...})`） | LSan 报**全部 indirect、没有 direct** 的泄漏（引用环特征），并连带拖住 runner / SequenceManager | 由**外部**持有流（测试作用域或成员 `shared_ptr`），回调只捕缓冲区等附属物；对象销毁前先 `Close()` 取消在飞操作 |
| **跨线程 PostTask 后无限等待** | 挂死（0% CPU，`futex_do_wait`） | `PostTask` 可能返回 false（runner 已停 / 已 shutdown）；返回 false 时**不能**再 `WaitableEvent.Wait()`，要么回退同步执行、要么返回失败 |
| **在对象所属 runner 之外的线程销毁该对象** | UAF / 卡在失效句柄或已销毁队列 | 销毁与 Close 一律 hop 到对象所属 runner（socket / 流 / 服务端都要求 I/O 线程碰），并在该任务内确认 teardown 完成 |
| **把 `TCPServerSocket::Close()` 与 `Shutdown()` 当同义词** | 断言失败或收到意外回调（`Close()` 会以 `success=false` 触发一次 accept 回调） | `Close()` = 通知调用方我关了（失败回调）；`Shutdown()` = 静默停止、不回调。收尾只想停用 `Shutdown()`；需要感知失败用 `Close()`，且回调里不要断言 `success` |
| **假设 ChildProcess 退出后 read 回调不会再来或一定成功** | 使用方若在回调里假定 `ok == true`，会按错误的前提处理数据 | 进程退出 / Close 时会以 `(false, 0)` 送达一次；调用方必须能处理失败路径，并在收到后停止重新发起读 |

**一句话总结**：库保证不会碰已释放的内部对象，但**不保证**调用方传入的回调所引用的东西还活着
—— 回调的捕获列表是调用方的责任。凡是要跨越函数返回的回调，一律「值捕获 heap 状态」，
并在返回前排空。

---

## 4. 教训清单（按模块）

### 任务调度 / 线程池

- **唤醒路径性能**（P2 主线）：丢失唤醒修复后 WSL dedicated SingleThread 投递
  -38~41%（pthread ERRORCHECK 锁每次握手 ~150ns）。spin-then-park 全部变体实测否决
  （WSL2 上 pause=VM exit、自旋无收益）；根因是每帖唤醒的锁+futex 握手，唯一避免的
  是 nofix 的无锁 broadcast（即丢唤醒 bug 本身）。**先决条件已落地：AtomicEvent**，
  下一步是替换 PooledTaskSource 的四个唤醒通道。
- **统一堆（Option B）**：sequenced 队列在 kAllowedNotSaturated 分支**不得** re-push
  （single-worker，re-push 的 queued=true 会堵死后续 enqueue）；dedicated 队列唤醒
  必须 `WakeDedicatedWorker()`（EnqueueTaskSource 对其 no-op）。
- **任何使 dedicated 队列可工作的路径（promote/re-enqueue/direct push）都必须显式唤醒
  owner**；否则延迟任务等 30s reclaim（~30s 停顿 == reclaim_timeout 信号）。
- **parallel runner 语义**：一个 PooledParallelTaskRunner 持**一个**多任务
  ParallelTaskSequence（每 post 一个 source 慢 3×）；worker 每次只 Take 一个任务
  （批量 take 会饿死其他 worker，破坏并行执行测试）；do NOT chase 旧 batch 实现的
  吞吐。
- **worker-repost 缺陷**（已修 a01dd2a）：parallel 队列经 TLS 本地队列注入会绕过
  WillRunTask → running_worker_count_ 负溢出 + 任务放大 + 死锁。
- **测量伪影**：parallel runner 完成判定不能只靠 FIFO sentinel + 立即读聚合计数
  （sentinel 只保证出队序，任务体可能仍在飞）；要等执行计数本身。
- **post_job**：完成检测 key off `assigned_workers_`（prev_assigned==1 && pending<=0），
  running 含 joiner 会竞态死锁；ShouldYield 收缩判断也用 assigned（不含 joiner）；
  joiner 空转每 8 迭代让步。Bench1 WSL 慢 9× 是平台 handoff 延迟（~2.3× Win），非 bug。
- **false-sharing**：单点 `alignas(64)` 隔离曾 -31%（WSL）；布局改动必须双平台同时段
  A/B（跨 2 小时 WSL 漂移可达 ±35%）。定向唤醒曾 -34%（per-queue mutex+condvar 贵）；
  去惊群的前提是定向唤醒本身廉价（现在有了 AtomicEvent，可重试）。
- **Selector work-bit deferred flush**：投递热路径的通知改原子标志 + DoWork 锁内合并
  刷新（MT 场景 -28%）。
- **Heap corruption 金律**：`lock_guard` 绝不能活得比其 mutex 的 `delete` 久
  （DestroySmallObjectAllocatorPartition：lock_guard 超出 delete → 解锁已释放 mutex）。
- **SequencedTaskQueue swap 快路径仅限单消费者**：PooledTaskQueue 多 worker 必须
  全锁保护 Take*（`set_single_consumer(bool)` 控制）。
- **ExecutionFence / ThreadPool Pimpl** 已落地；可选遗留：`GetCurrentTaskImportance`
  继承、`CreateSequencedTaskRunnerForResource(path)`。
- **Log TLS 缓存**必须 key by handle + config snapshot version（slot 运行期复用）。
- **TaskRunner 持的是裸 queue 指针**：`SequencedTaskRunner::Impl` 指向 `SequenceManager` 拥有的
  `SequencedTaskQueue`，而 `Thread::ThreadMain` 里的 `SequenceManager` 是**线程栈对象**——线程
  退出（run loop 返回）后它连同队列一起销毁，此后任何 `runner->PostTask()` 都是 UAF。
  2026-10-18 在 RelWithDebInfo+ASAN 下见过一次 `Thread::Stop()` 路径的队列 UAF（`PeekNextDelayedRunTime`
  读到已释放队列，另一线程正在析构 `queues_`），其后 55 轮未再复现，但**设计风险仍在**：
  线程退出后旧 runner 必须视为失效，不能缓存后跨线程复用。待更多证据后再收紧（候选：queue 改
  refcounted，或 runner 持 WeakPtr + 失效判定）。

### IO / 网络

- **AsyncFile 关闭协议**：Close 后在途 OVERLAPPED 不能被释放——等 IOCP 取消完成包
  排空再关句柄；`~Impl` DCHECK 状态必须 kDisconnected（先 Close 完成再销毁）。
- **TCPClientSocket Orphan**：不可 shutdown+drain 等对端 FIN（双方互等挂死）；
  flush-then-close；硬拆加显式 `Abort()`。socket 只能 IO 线程碰。
- **POSIX accept backlog**：close(listen_fd) 不 RST 已完成连接 → 关前
  DrainAcceptBacklogLocked；accept 循环 closed 分支 continue drain。
- **POSIX connect EINPROGRESS 边缘竞态**：StartWatching 后立即重查 SO_ERROR
  （epoll 注册前握手完成会丢事件）。
- **Windows accept pool 泄漏**：Shutdown/Close 不能 StopWatching 抹掉 pump watch
  （pending AcceptEx 的 ABORTED 完成包会丢）；让 OnIOCompleted 排空。
- **PipeStream**：Windows 命名管道 `ERROR_MORE_DATA` 是分包未完而非失败；
  管道名 `pid+GetTickCount64()` 不够唯一，加进程内计数器。
- **AsyncLineReader**：大文本用 consume_offset 游标批量压缩（erase(0,n) 是 O(N²)）；
  析构禁止同步触发用户回调。
- **IOBufferPool**：`AcquireBuffer` 返回桶规格化尺寸（size()≠请求长度）——帧/语义长度
  必须用精确 `new IOBufferWithSize(n)`；`PooledIOBuffer` 只暴露 capacity()（类型级隔离）。
- **SSLContext 契约**：普通值对象；socket 持非拥有 `SSLContext*`，**调用者保证 context
  活到所有 socket 结束**（违反即 UAF）。每份二进制副本用 mbedtls 需
  `EnsureMbedtlsThreading()`；`MBEDTLS_DEBUG_C` 已关闭（异步 teardown 经 `ssl->conf`
  f_dbg 的 UAF）。
- **单 IO 线程足够**：1000 连接下单 epoll/IOCP 高效；多 reactor 池化实测有害
  （调度/同步开销 > 派发收益），按需显式多实例即可。
- **跨线程 hop 的「调用时快照 + 执行时使用」必须执行时重校验**（2026-10-18，库缺陷，`11bcc35`）：
  `child_process_stream_proxy` 的 `ReadAsync()` 在调用线程快照目标流指针，hop 任务在 I/O 线程才
  `raw_target->ReadAsync(...)`；而宿主线程可能已先投递了 `ShutdownOnIoThread`（两者竞争同一队列
  顺序）→ 目标已释放则 UAF。修法不是「再判一次空指针」而是：把 target/closed 放进**引用计数令牌**
  （`InputStreamBinding`/`OutputStreamBinding`，hop 按值持有），hop 内校验
  `!closed && target == raw_target`。**为什么不用 `WeakPtr::get()`**：它只读失效标记、不保命，
  两次读之间对象可被释放（TOCTOU）；引用计数令牌本身就把校验数据保住了。

### HTTP / HTTP2 / WebSocket

- **路由表与连接注册表**放 `SharedState : RefCountedThreadSafe`（.cpp 完整定义），
  Connection/accept 回调持强引用；注册表持强引用（显式 AddRef/Release，Release 在锁外
  防重入）；Shutdown 锁内快照 scoped_refptr 再逐个 Close；accept 注册时锁内重查
  accepting 标志。
- **HttpClient 完成路径**：先 move 清空 callback 成员再调用（池复用下与并发 Send 的
  写竞争）；非 keep-alive 用 `state.exchange(kClosed)` 认领防双 Finish。
- **h2 服务端**：RST 不能在 nghttp2 回调内提交（UAF），记录意图 mem_recv 后统一提交；
  hook 后不得再读流字段（先拷贝 id/path）；GOAWAY 用 last_stream_id=已见最大；
  排水 watchdog 必须 WeakPtr 捕获（强引用钉住对象 30s 且在无关线程析构）。
- **h2 多流并发**：每流独立 active/generation；流关闭回调置 active false。
- **句柄（客户端/服务端）协议**：共享 `atomic<bool>` active + generation + WeakPtr，
  不持有连接；任意线程操作 hop 到 I/O 线程；同对象跨线程并发需先拷贝再使用；
  Cancel 后 respond/write/write_io/close 全部 no-op（已测试重放）。
- **llhttp 9.x**：`on_status` 传 reason phrase；HPE_PAUSED_UPGRADE 时 Execute 返回
  已消费全部头块（consumed=0 会回显 WS 帧）。
- **h1 POST 无 Content-Length**：llhttp 把 body 字节当下一请求解析——测试必须显式加
  Content-Length。
- **HttpClientPool**：Flush() 不可在归还回调内同步调用（重入崩溃），需延迟任务；
  idle timeout（30s）+ Acquire 时 `Peek()` 探活已落地。

### IPC / 进程 / 日志

- **IPC（MessageChannel/RpcEndpoint）**：跨线程回调用 RefCountedThreadSafe 自持
  （WeakPtr 跨线程在 debug 下 FATAL）；`std::mutex` 不可重入——锁内决策锁外调用；
  帧构建用精确尺寸 IOBuffer。
- **ChildProcess**：fork→exec 之间只允许 async-signal-safe（堆分配逻辑移 fork 前）；
  流代理必须显式传 io_task_runner 与 caller runner 两条序列；target==io 时直接内联
  回调避免自投递饥饿。
- **流代理的关闭顺序就是契约**（2026-10-18）：`ShutdownOnIoThread()` 必须**先** `ResetBinding()`
  （把 target 置空）**再** `core_.reset()` 销毁真管流。hop 任务靠这个顺序在执行时判定「绑定已
  失效 → 回调 `(false, 0)`」；顺序一改，析构时的在飞读就又会退化成 UAF。同理 `Bind()` 必须在
  任何用户 `ReadAsync` 之前完成。
- **子进程在飞读与宿主析构的竞争**（`11bcc35`）：读完成回调会在 I/O 线程重新发起下一次读
  （此时 `closed_` 仍为 false），而宿主线程同时析构 `ChildProcess`；两者 `PostTask` 到同一队列的
  顺序**无法保证**。所以关闭路径不能只靠「队列 FIFO 天然有序」，必须有执行时重校验。
- **`Launch()`/`Terminate()` 必须检查 `PostTask` 返回值**：服务 runner 已停时投递会失败，
  旧代码会永久阻塞在 `done.Wait()`（已修为返回失败；`Shutdown()` 本就是该语义）。
- **日志**：`write_pos` 是预留槽位不是已提交数——drain 等待用连续发布的
  `committed_pos` 前缀；TSan 竞态：initialized 全原子化、stop_requested 写在
  pthread_create 之前。
- **g_nei_logger**：库诊断默认静默、`nei_enable_diagnostic_message_to_stdout` 按需开；
  导出底层 `uintptr_t` 类型避免头文件暴露 log.h。

### 测试 / 诊断

- **valgrind 会死锁时序敏感的并发测试**（missed wakeup → 无限 hang）——必须 `timeout`
  包裹且只扫 UAF/OOB；并发正确性用 TSan。
- **逻辑时序竞态 TSan 检不出**（如 condvar 丢唤醒）——TSan 清零不代表无 bug，要审协议。
- **ASan 的 UAR 检测默认关闭**：必须 `ASAN_OPTIONS=detect_stack_use_after_return=1`，且用最小
  探针自证检测生效，否则跑多少轮都无信息量。UAR 的真实成因几乎总是**回调捕栈**，且只在
  「函数返回后仍有在飞回调」时偶发（实测约 1/10 全量轮次）。
- **LSan 报「全部 indirect、没有 direct」= 引用环**：这是判读技巧，不是噪声。典型来源是
  对象持有回调、回调又持同一对象的 `shared_ptr`。修法是拆环（外部持有 + 销毁前 Close）。
- **gtest 的 `TearDown` 在 `TestBody` 返回之后才跑**：所以 TearDown 里 drain runner 保护不了
  TestBody 的栈局部，必须由测试体自己在返回前排空。这解释了为何「代码审查看不出嫌疑」
  却仍报 `stack-use-after-return`。
- **收尾 helper 范式（本轮沉淀）**：`CloseServerOnIoThreadAndDrain(server)` /
  `CloseStreamOnIoThreadAndDrain(stream)` —— 在对象所属 runner 上 Close + 析构，再从**同一任务
  内部**投 fence 并等待。注意 `TCPServerSocket` 收尾要用 `Shutdown()`，`Close()` 会以
  `success=false` 触发 accept 回调，打破用例已做的成功断言。
- **精度/性能类断言用最小值或分位数，绝不用均值**：`SleepShortDurationPrecision` 原来用 5 次
  平均对比 8ms，单个被抢占的样本就能把均值拉过界（实测均值 10.5ms / 14.4ms，而每次 sleep 本身
  正常），于是 Windows 全量每轮偶发 1 次。改取 10 次最小值后：抢占只会让单次变长、不会让最小值
  变小，而真实的定时器精度回退会让**每一次**都对齐到 tick（≈15.6ms），判据强度不变且对抖动免疫。
  计时一律用 `std::chrono::steady_clock`（`high_resolution_clock` 在 MSVC 下非单调，会出现负值）。
- **flaky 已修（2026-10 收口，全平台全量零失败）**：`ClientOrphanDrainReadEOF`（server 析构 +
  I/O 线程 accept 回调碰已析构栈事件 → futex 挂死）；`ServerDoesNotCrashUnderFdPressure` /
  `WriteChainNoStackOverflow`（`7ca30ad`，回调捕 TestBody 栈 → ASAN stack-use-after-return，
  同类用例已全量清理）；`PlatformThreadTest.SleepShortDurationPrecision`（`735552e`，均值断言
  → 最小值断言）；`ThreadPoolTest.DelayedTaskRunsWithoutImmediateKick`。
- **新功能落地后必须重跑消毒器**——旧 ASAN/valgrind 基线不覆盖后续功能
  （HttpServerRequestHandle 教训：功能后补跑 ASAN 912、TSan 891、valgrind 定向）。
- **后台 WSL 任务疑似挂起**：查 `loadavg`（0.00=阻塞）、`stat -c %y <log>`（mtime 冻结）、
  `pgrep -af` 全命令行（comm 会漏 valgrind.bin）。注意 WSL 重启会清空 `/tmp`，取证日志要及时
  转存到仓库目录。

---

## 5. 模块状态快照（2026-10-18）

**已完成且验证充分**：
- 任务中枢：ThreadPool（Pimpl/单例/ExecutionFence/优先级/延迟时间轮/may_block）、
  SequenceManager（fast-path + deferred flush）、PostJob、Timer/RepeatingTimer、
  Bind/BindOnce/BindPostTask、Thread/MessagePumpForIO（IOCP/epoll）、IOThread 单例。
- 同步：Lock/ConditionVariable/WaitableEvent/**AtomicEvent**（新）。
- 内存/生命周期：scoped_refptr/WeakPtr/NoDestructor/Singleton/AtExitManager/
  SmallObjectAllocator/SharedMemory（全量封印）。
- IO：AsyncFile（IOCP/背景线程）、PipeStream（写队列）、AsyncLineReader、
  StreamReader/Writer、IOBufferPool。
- 网络：TCP/UDP/TLS（mbedtls 3.6.3 + ALPN）、TCPServerSocket 多 reactor、
  HostResolver（c-ares）、Keep-Alive 双层。
- HTTP：HttpServer（h1+h2 单端口 ALPN、流式双向、背压、模式路由、WebSocket）、
  HttpClient（h1/h2 融合、keep-alive、流式上传/下载、HttpFileTransfer）、
  HttpClientPool、HttpRequestHandle + HttpServerRequestHandle（Cancel/SetPriority）。
- IPC：MessageChannel/RpcEndpoint；进程：ChildProcess/ProcessService。
- C 层：日志（MPSC 无锁环）、encoding、路径、系统信息、flake_id、crypto。

**2026-08 之后新增**（同一验证标准）：`native_library`（裸句柄 + RAII）、HTTP 中间件
/ 重定向自动跟随 / gzip-deflate / Cookie / multipart、`StorageMonitor`（Linux mountinfo +
Windows CM_Notify/WM_DEVICECHANGE 双路径、`EjectDevice`）、HttpClientPool idle 探活、
bench 统一清单与双平台 runner。

**未完成/候选**（详见 docs/TODO.md）：
- P3：TaskQueueSelector cache-line 布局、PipeStream direct dispatch、CMake 检测审查。
- HTTP 功能补全：中间件；HttpClient 自动跟随重定向（组件已就绪）。
- Future：Storage Device Monitoring（自研）。

---

## 6. 当前最优先下一步

1. **5b：用 AtomicEvent 替换 PooledTaskSource 四个唤醒通道**（global cv / dedicated
   cv-Broadcast / SharedWorker event / DelayedManager event），验收：WSL dedicated
   ≥3.8M/s（nofix=4.08、当前=2.52），TSan/全量不回归。
2. 句柄功能收尾已全部完成；可选：`GetCurrentTaskImportance` 继承、
   `CreateSequencedTaskRunnerForResource`。
3. 质量：**已完成（2026-10-18）**——WSL ASAN 全量兜底、四类 flaky 收口、Known Flaky 表
   归档，当前 Windows Debug 1064 / Release 1058 / WSL Release 1054 / WSL ASAN 1060 全零失败。
   下一个待观测项：TaskRunner 持裸 queue 指针的潜在 UAF（见任务调度章节，暂无复现记录）。
