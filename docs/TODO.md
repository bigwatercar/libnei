# libnei — TODO & Roadmap

**Updated**: 2026-07-22

---

## Log Module

All critical issues resolved (lock-free MPSC ring buffer, snapshot-based config, adaptive signal/broadcast).
Remaining P3 items:
- #7 Cache-line false sharing (long-term)
- #9 User callback lock ordering docs (long-term)
- Crash handler: non-async-signal-safe in POSIX signal handlers (accepted limitation)

---

## PipeStream — Known WSL Issues (4 tests disabled)

Root cause: IO pump task queue wake-up race.  When I/O completes synchronously,
the pump may enter `epoll_wait` / `GetQueuedCompletionStatus` before the posted
callback is processed.

| Test | Symptom | Direction |
|------|---------|-----------|
| `PosixYieldQuotaPreventsStarvation` | Reader hangs | Verify on native Linux |
| `WriteToDisconnectedPeerFailsCleanly` | Write callback never fires | Exceed pipe buffer to force EPIPE |
| `RapidCancelAndRetryStateMachine` | Phase 2 read hangs | Pump task ordering on WSL |
| `RapidWriteCancelAndRetry` | Same as above, write direction | Same |

Action: run on native Linux; gate WSL-failing tests with `nei::IsRunningOnWsl()`.

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

---

## Future Feature Directions — Evaluated 2026-07-22

Chromium reference analysis for four candidate features.  Not yet scheduled.

### 1. File System Monitoring (`FilePathWatcher`)

**Chromium**: `base/files/file_path_watcher.h` (PIMPL + PlatformDelegate)
- Linux: inotify, macOS: FSEvents+kqueue, Windows: `ReadDirectoryChangesW`+IOCP
- Recursive / non-recursive, per-file change type

**libnei**: ★★☆ — No deps, platform APIs clear, natural fit for `neixx/io/`.

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
