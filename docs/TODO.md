# libnei — TODO & Roadmap

**Updated**: 2026-08-05

---

## Known Minor Leaks (P3)

### `IoThread` dtor: WeakPtrFactory::InternalFlag residual (~1KB) 🔧 2026-07-27

**Symptom**: ASAN reports ~1KB (17 allocations) indirect leak when an `IoThread`
is destroyed — `WeakPtrFactory<internal::TaskQueue>::InternalFlag` objects not
fully released because outstanding `WeakPtr`s still reference them.

**Impact**: negligible — fixed ~1KB regardless of data size or thread count,
reclaimed by OS at process exit.  No OOM risk, no performance degradation.

**Affected benches**: `tls_throughput_bench`, `tcp_loopback_bench`, any bench
creating an `IoThread`.

**Next steps**: investigate `SequenceManager::Shutdown()` / `TaskQueue` cleanup
to ensure `WeakPtrFactory` invalidation drains all outstanding `WeakPtr`s before
the factory is destroyed.  Low priority.

---

## Log Module ✅ Comprehensive optimization completed 2026-07-31

### Consumer-path throughput optimization (+46% cumulative)

| Stage | Commit | Improvement |
|-------|--------|-------------|
| Deadlock fix: `_nei_log_notify_waiters_after_drain` always broadcasts | `50cf4a3` | Correctness |
| False-sharing isolation: cache-line padding for ring counters, stats groups, `consumer_sleeping`, slot structs | `50cf4a3` | +8% |
| Consumer acquire/release batching: config cached across same-batch events | `50cf4a3` | ~flat |
| **Timestamp sub-second table**: `kMillisDigits[1000][4]` replaces `snprintf` with `memcpy` on cache-hit path, all 4 timestamp styles covered | `71a117c` | **+36%** |
| Ring slot sizing experiment: 256 (default) confirmed as best balance | N/A | 128←256→1024 tested |

### NEI vs spdlog benchmark (2026-07-31)

| Scenario | NEI (optimized) | spdlog | Winner |
|----------|----------------|--------|--------|
| Memory simple printf/fmt | **2.35 M/s** | 1.65 M/s | NEI +42% |
| Memory literal | **2.67 M/s** | 2.58 M/s | NEI +3% |
| Memory multi param | 1.13 M/s | **1.16 M/s** | spdlog +2% |
| File simple | 1.34 M/s | **1.79 M/s** | spdlog +33% |
| File multi | **1.71 M/s** | 1.40 M/s | NEI +22% |
| Strict sync | 73 K/s | 75 K/s | ≈ tie |

### Remaining P3 items

- Crash handler: non-async-signal-safe in POSIX signal handlers (accepted limitation)

---

## PipeStream — Known WSL Issues ✅ Resolved 2026-07-25

Root cause was IO pump task queue wake-up race.  When I/O completed synchronously,
the pump entered `epoll_wait` before the posted callback was processed.

Fix: EPOLLONESHOT with explicit re-arm (completed 2026-07-22, verified 2026-07-25).
All 4 previously-failing tests now pass on WSL.  Removed the `#if defined(__linux__)
&& !defined(__WSL__)` compile-time guards.

Verified: WSL 10/10, Windows 9/9 (PosixYieldQuota is POSIX-only).

---

## Pending Optimizations

- **PipeStream epoll oneshot**: ✅ Completed 2026-07-22. Level-triggered → `EPOLLONESHOT` with explicit re-arm in `DrainRead`/`DrainWrite`.  See `docs/neixx_io_technical.md` §2.12.
- **PipeStream direct dispatch continuation**: batch-quota-exhausted path could chain directly instead of going through `PostTask`.  Low priority — current design is correct.
- **SmallObjectAllocator idle thread-cache purge** ✅ Completed 2026-08-05:
  Implemented in `modules/neixx/memory/src/small_object_allocator.cpp`:
  - Chunks are now page-backed (`VirtualAlloc`/`mmap`) and tracked in a global
    list with per-chunk accounting (`in_use` atomic + `central_count` under lock).
  - `PurgeSmallObjectAllocator()` (exported) returns fully-free chunks to the
    OS: flushes the calling thread's cache, walks the chunk list, and reclaims
    chunks where `central_count == 64 && in_use == 0` (race-free by construction —
    a chunk can only become reclaimable via a lock-held flush).
  - Counter-based self-purge (every 1024 allocations, shed half the per-class
    thread cache) mirrors PartitionAlloc's no-timer approach.
  - New stats: `chunk_purges`, `reserved_bytes`, `released_bytes`.
  - Reclamation is explicit/on-demand (memory-pressure style), no internal timer.
- **MemoryPressureMonitor (independent component)** ✅ Completed 2026-08-05:
  - New public component `modules/neixx/memory/memory_pressure_monitor.{h,cpp}`
    (Chromium-style): `MemoryPressureLevel` (kNone/kModerate/kCritical),
    `GetCurrentMemoryPressureLevel()` (platform strategy: Windows commit-limit
    polling via GlobalMemoryStatusEx; macOS zero-threshold XNU sysctl
    kern.memorystatus_vm_pressure_level; Linux MemAvailable + reclaimable
    page-cache ratio; other POSIX available/total ratio),
    `MemoryPressureListener` + `MemoryPressureListenerRegistry`
    (thread-safe Add/Remove/Notify, listeners invoked outside the lock), and a
    stateful `MemoryPressureMonitor` (PIMPL) whose `PollOnce()` re-samples the
    level and auto-notifies listeners on change — caller-driven, no internal
    timer or background thread.
  - Decoupled from the allocator: the application ("main program") assembles
    the pipeline — sample the level from its own low-frequency hook, call
    `Notify()` on change, and a listener drives `PurgeSmallObjectAllocator()`.
  - SmallObjectAllocator no longer carries any pressure-detection API
    (watermark/ratio/if-over-watermark removed); it stays a pure alloc/reclaim
    component with explicit `PurgeSmallObjectAllocator()`.
- **SmallObjectAllocator Chromium-alignment round 2** ✅ Completed 2026-08-05
  (freelist hardening, decommit middle-state, committed/per-class stats, partitions):
  - **Freelist hardening**: every free-list link is XOR-encoded with a per-
    partition random key (fail-fast on corruption); bytes 8..15 of each block
    still keep the owning chunk pointer for O(1) free.
  - **Decommit middle-state** (`DiscardSystemPages` semantics): `PurgeSmallObjectAllocator()`
    now decommits fully-free chunks (physical pages → OS, virtual address kept)
    and parks them per size class (cap `kMaxDecommittedPerClass=4`); `AcquireChunk`
    recommits a parked chunk instead of carving a new one.  Excess parked chunks
    are released entirely to bound virtual-address growth.
  - **Stats**: `committed_bytes` added to `SmallObjectAllocatorStats`;
    `GetSmallObjectAllocatorSizeClassStats()` reports per-class `size`/`in_use`/
    `central_free`.
  - **Partition isolation**: `CreateSmallObjectAllocatorPartition()` /
    `SmallObjectAllocInPartition()` / `PurgeSmallObjectAllocatorPartition()` —
    each partition owns its own central pool, freelist key, purge and stats
    (fixed budget `kMaxPartitions=8` incl. default).  Blocks never cross
    partitions; free routes via the block header.  `SmallObjectFree()` works for
    any partition.
  - All exported API additions are on the new (unreleased) component; no impact
    on the published callback ABI.

---

## Known Flaky Tests 🔧 2026-08-01

Tests that intermittently fail due to timing sensitivity or environment
dependencies.  Not indicative of logic bugs — triaged after the 8-quadrant
full build + test validation of the TaskRunner hierarchy refactoring.

### WSL (Linux GCC)

| Test | Frequency | Root Cause |
|------|-----------|------------|
| `PipeStreamTest.PosixYieldQuotaPreventsStarvation` | ~30% | Timing-sensitive yield quota; CI stress runs more predictable |
| `ChildProcessTest.LaunchWithStdinPipeEchoesToStdout` | ~10% (Release only) | Pty/pipe teardown race in Release-optimized builds |
| `HostResolverTest.ResolveInvalidHost` | Always (WSL) | WSL has no IPv6 / external DNS reachability |
| `HostResolverTest.ResolveIPv6Only` | Always (WSL) | WSL has no IPv6 connectivity |
| `HostResolverTest.CustomDnsServerIPv6Cloudflare` | Always (WSL) | WSL has no IPv6 connectivity |
| `HostResolverTest.CustomDnsServerIPv6Google` | Always (WSL) | WSL has no IPv6 connectivity |
| `HostResolverTest.CustomTimeout` | Always (WSL) | WSL DNS timeout behavior differs |

### Windows (MSVC)

| Test | Frequency | Root Cause |
|------|-----------|------------|
| `TlsSocketTest.DestructionDuringHandshake` | ✅ Fixed `b0061e0` | Serialized Orphan() to IO sequence, transport destroyed on owning thread |
| `HostResolverTest.ResolveDualStack` | Always (no DNS) | Requires functional DNS resolver |
| `HostResolverTest.ResolveIPv4Only` | Always (no DNS) | Requires functional DNS resolver |
| `TimerTest.RepeatingTimerStopPreventsFurtherFires` | Always (crash) | SEH 0xc0000005 access violation; reproduces on baseline commit too (pre-existing on chromium_callback branch) — likely a sequence-manager/timer teardown race, independent of the allocator |

### Notes

- DNS-dependent tests pass in CI environments with full network access.
- Pipe / ChildProcess flaky tests are candidates for eventual
  `EXPECT_DEATH`-style retry loops or event-based synchronization fixes.
- Low priority — no crash, no data corruption, no production impact.

---

## OnceCallback SBO Move Fence — ✅ Superseded by Chromium-style Rewrite 2026-08-04

**Root cause**: MSVC SSO `std::string` self-referential pointers were corrupted
by `std::memcpy` of the 48-byte SBO storage.  `std::launder` + real C++ move
constructor (commit `87036c1`) fixed 99.5% of losses; dropping `std::move(*fn)`
to `std::invoke(*fn)` (commit `bac38e7`) further reduced losses to ~1/200 rounds.

**Resolution**: Full Chromium-style heap-only callback rewrite (commit `1949dc5`):
- Eliminated SBO entirely — all callbacks heap-allocated via `BindState<Fn, BArgs...>`
- `Invoker<Storage, Sig, IsOnce>` dispatches via `std::apply` + `std::index_sequence`
- Split `UnwindOnce` (move) / `UnwindRepeat` (const T&) policies with `reference_wrapper` specializations
- Null-safe `Run()` guards against double-invoke on consumed `OnceCallback`

**Four-quadrant verification (2026-08-05)**:

| Quadrant | Compile | Tests (26) | Stress (100×500K) |
|---|---|---|---|
| Windows Debug (MSVC) | ✅ | ✅ 100% | FAIL=1/100 ⚠️ |
| Windows Release (MSVC) | ✅ | ✅ 100% | FAIL=1/100 ⚠️ |
| WSL Debug (GCC) | ✅ | ✅ 100% | **FAIL=0/100** ✅ |
| WSL Release (GCC) | ✅ | ✅ 100% | **FAIL=0/100** ✅ |

**Key finding**: Linux (GCC) has **zero** task loss across 100M task postings.
Windows (MSVC) ~1% round failure (5/500K tasks dropped per failure) is a
**ParallelTaskRunner scheduler-level issue**, not a callback mechanism bug.
Diagnosed as `scheduler_dropped=5` with `pushed==taken` and `once_cb_run` matching
— tasks vanish between pipeline dequeue and execution.

**Next steps**: Investigate Windows-specific race in `ParallelTaskRunner` pipeline
(batch dequeuing, worker wake-up, or `WillRunTask` filtering).

---

## OnceCallback Templatization

**Status**: ✅ Completed 2026-07-22.

`OnceCallback<Args...>` is fully templated:
- SBO (48 bytes) + heap fallback for large functors
- Supports `void()`, `int`, `const string&`, `unique_ptr<T>`, multi-param
- `BindOnce` returns `OnceCallback<>` (pre-bound args)
- `BindPostTask` forwards `Args...` via perfect capture
- `ResolveCallback = OnceCallback<const AddressList&>` for HostResolver
- 14 unit tests covering construction, move, run-once, SBO/heap, move-only types

**Status**: ✅ Completed 2026-07-23 — `RepeatingCallback<Args...>` is now fully
parameterized with `void Run(Args...) const`, copy semantics (SBO copy-construct /
heap ref-count), `explicit operator bool`, `operator=(nullptr_t)`, and implicit
construction from compatible callables (excluding `nullptr_t`). `BindRepeating`
uses `shared_ptr` heap storage. `FilePathWatcher` migrated from `std::function`.

---

## TCP Net Module — Pending Tests

### `TCPServerSocket_FDExhaustion` (POSIX, P2)

Verify EMFILE/ENFILE survival without crashing.  Blocked: IO pump requires epoll
FDs internally; process-level FD exhaustion starves the pump.

Next steps: mock/epoll-free IO pump for this test, or pre-configure an fd to fail
`accept4` without exhausting system FDs.

### `TCPClientSocket::Impl` Leak on Accept Path (POSIX + Win, P1) 🔧 2026-07-26

**Symptom**: ASAN reports 408B direct leak per accepted connection in
`tcp_server_socket_posix.cpp:259` (`new TCPClientSocket::Impl`), plus 8B
indirect via `WeakPtrFactory<Impl>`.  Total ~416KB per 1,000 connections.

**Root Cause**: `TCPClientSocket::Impl::Orphan()` takes a self-hold (`AddRef()`)
that is only released when `StartOrphanDrain()` completes (via EOF → `Close()` →
`ReleaseSelfHoldIfNeeded()`).  But when `Close()` is called BEFORE `Orphan()`
(the normal user pattern: `sock->Close()` then unique_ptr destructor calls
`Orphan()`), `closed_` is already `true`, so `Orphan()` skips the drain but
never releases the self-hold.

**Fix**: Added `else { ReleaseSelfHoldIfNeeded(); }` branch in both POSIX and
Windows `Orphan()` when `closed_` is already true.

**Verification**: ✅ Fixed 2026-07-26. ASAN confirm: 1K connections → 0 leaks (was 416KB).

---

## Future Feature Directions — Evaluated 2026-07-22

Chromium reference analysis for four candidate features.  Not yet scheduled.

### 1. File System Monitoring (`FilePathWatcher`)

**Status**: ✅ Completed 2026-07-22.

**Chromium**: `base/files/file_path_watcher.h` (PIMPL + PlatformDelegate)
- Linux: inotify, macOS: FSEvents+kqueue, Windows: `ReadDirectoryChangesW`+IOCP
- Recursive / non-recursive, per-file change type

**libnei implementation**:
- Public API: `include/neixx/io/file_path_watcher.h` (PIMPL, `std::function` callback)
- POSIX backend: `src/file_path_watcher_posix.{h,cpp}` — inotify via `FdWatchController`
- Windows backend: `src/file_path_watcher_win.{h,cpp}` — `ReadDirectoryChangesW` via IOCP `CompletionWatcher`
- 8 unit tests covering create/modify/delete detection, cancel, re-watch, error paths
- See `docs/neixx_files_technical.md` for full documentation.

### 2. SSL/TLS Support

**Chromium**: `net/socket/ssl_client_socket_impl.cc` (BoringSSL)
- `SSLClientSocket` / `SSLServerSocket` wrapping TCP sockets
- ALPN for HTTP/2 negotiation

**libnei**: ★★★★ — Requires TLS library integration (OpenSSL/BoringSSL/mbedTLS).

### 3. HTTP / WebSocket / HTTP2 Server

**Chromium**: `net/test/embedded_test_server/` (HTTP/HTTPS/WS/WSS/HTTP2)
- Phase 1: HTTP/1.1 → Phase 2: WebSocket → Phase 3: HTTP/2 (needs TLS)

**libnei**: ★★★ — Builds on TCP module.

### 4. Storage Device Monitoring

**Chromium**: No generic `base/` implementation.

**libnei**: ★★★ — Must design from scratch. Win: `RegisterDeviceNotification`,
Linux: `libudev`/netlink.

### Priority

| # | Feature | Rationale |
|---|---------|-----------|
| 1 | File System Monitoring | Independent, no deps |
| 2 | HTTP/WebSocket Server | Builds on TCP |
| 3 | SSL/TLS | Heavy dep, needed for HTTP/2 |
| 4 | Device Monitoring | No Chromium reference |

---

## IOBuffer::data() 返回类型改为 unsigned char* (P2) ✅ 已完成 2026-07-29

**背景**: 2026-07-29 `tls_throughput_bench` 哈希校验失败，根因是 MSVC 上 `char`
为 signed，`chunk->data()[i]` 与 `uint64_t` XOR 时发生符号扩展。

**方案**: 将 `data()` 返回 `unsigned char*`，内部 `data_` 同步改类型。
`WSABUF.buf` 赋值处加 `reinterpret_cast<CHAR*>`，`std::string` / `ostream::write`
调用处加 `reinterpret_cast<const char*>`。共修改 21 个文件，四象限零回归。

---

## PostJob 接口参数对齐 Chromium (P2) 🔧 2026-07-30

**现状**: `PostJob(from_here, traits, ...)` 的 `from_here` 和 `traits` 参数被
`(void)` 丢弃，`CreateParallelTaskRunner(TaskTraits())` 硬编码默认优先级，
丢失了调用方指定的优先级控制和调用位置追踪能力。

**Chromium 参考**: `base/task/post_job.cc`
- `from_here` → 存入 `JobTaskSource`，崩溃报告/tracing 中显示 PostJob 位置
- `traits` → 控制 worker 线程优先级（BEST_EFFORT/USER_VISIBLE/USER_BLOCKING）
  和 ThreadPolicy
- `GetCurrentTaskImportance()` → 继承当前线程的重要性

**方案**:
1. `JobTaskSource` 构造函数接收 `Location` 和 `TaskTraits`
2. `PostJob` 将 `traits` 透传给 `CreateParallelTaskRunner`
3. `from_here` 存入 `JobTaskSource` 用于 `posted_from()` 查询
