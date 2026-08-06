# libnei — TODO & Roadmap

**Updated**: 2026-08-06

---

## 待办事项（未完成）

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
| `TimerTest.RepeatingTimerStopPreventsFurtherFires` | Always (crash) | SEH 0xc0000005；基线复现（chromium_callback 分支既有）— 疑 sequence-manager/timer teardown race，与 allocator 无关 |

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
- **PipeStream WSL 问题** 2026-07-25 — EPOLLONESHOT 显式 re-arm。
- **IOBuffer::data() 类型** 2026-07-29 — `char*`→`unsigned char*`（符号扩展 bug）。
- **FilePathWatcher** 2026-07-22 — inotify / ReadDirectoryChangesW。
- **TCPClientSocket::Impl Accept 路径泄漏** 2026-07-26 — Orphan() 加 `ReleaseSelfHoldIfNeeded()`。
- **OnceCallback 系列** 2026-07/08 — 模板化 + Chromium 风格 heap-only 重写（RefCountedThreadSafe + extern template ABI）。
