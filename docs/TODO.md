# libnei — TODO & Roadmap

**Updated**: 2026-07-27

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

## Log Module

All critical issues resolved (lock-free MPSC ring buffer, snapshot-based config, adaptive signal/broadcast).
Remaining P3 items:
- #7 Cache-line false sharing (long-term)
- #9 User callback lock ordering docs (long-term)
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

## IOBuffer::data() 返回类型改为 unsigned char* (P2)

**背景**: 2026-07-29 `tls_throughput_bench` 哈希校验失败，根因是 MSVC 上 `char`
为 signed，`chunk->data()[i]` 与 `uint64_t` XOR 时发生符号扩展。修复方式是在调用处
加 `static_cast<unsigned char>()`。治本方案是将 `IOBuffer::data()` 返回类型从
`char*` 改为 `unsigned char*`，从 API 层面消除此类 bug。

**影响范围**: ~111 处引用，主要是 `memcpy`/`ReadFile`/`WriteFile`（接受 `void*`，
无需 cast）和少量 `std::string::assign` / `std::string_view`（需要 cast）。

**方案**: 将 `data()` 和 `data() const` 返回 `unsigned char*` / `const unsigned char*`，
内部 `data_` 成员同步改类型。现有调用处若编译报错，加 `reinterpret_cast<const char*>()`。
