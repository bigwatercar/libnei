# libnei - TODO & Roadmap

**Version**: v3.0 | **Date**: 2026-06-12

---

## Log Module - Status

| ID | Description | Status |
|----|------------|--------|
| #1 | Global mutex serializes enqueue | OK Lock-free MPSC ring buffer |
| #2 | memcpy under mutex in enqueue | OK Eliminated by ring buffer |
| #3 | Double-buffer backpressure | OK 256-slot ring buffer |
| #4,#6 | Thundering herd on flush/wake | OK Adaptive signal/broadcast |
| #5 | RWLock per log in consumer | OK TLS cache + snapshot |
| #8 | Flush deadlock in consumer thread | OK Consumer-thread detection |
| #10 | Config TOCTOU UAF | OK Snapshot + write-lock + update_config |
| #11 | Lazy init race under read-lock | OK Converged init + handle-only |
| B | Coarse RWLock write section | OK Slot-only protection |
| A | consuming_index long wait | Obsolete after refactor |

### Known Limitations

- Crash handler: non-async-signal-safe in POSIX signal handlers (accepted as best-effort).
- In-place config changes require manual `nei_log_update_config()`.
- #7 Cache-line false sharing (P3, long-term).
- #9 User callback lock ordering docs (P3, long-term).

---

## 2026-06 Key Progress

1. **Sink release lifecycle**: `release` callback, `nei_log_remove_config` dual-flush, `nei_log_shutdown`.
2. **Config in-place modify**: `nei_log_update_config()` snapshot bump.
3. **API polishing**: `add_sink`/`remove_sink`/`release_sink` uniform naming.
4. **Console output**: `log_to_console` removed, replaced by `nei_log_create_stdout_sink()`.
5. **Auto-flush**: consumer idle-wake timer flushes pending sink data for real-time visibility.
6. **Architecture**: mutex+double-buffer -> lock-free MPSC ring; broadcast -> adaptive signal/broadcast.
7. **Config caching**: producer/consumer use TLS cache + snapshot version.

---

## PipeStream Unit Tests — Known WSL Issues

The following 4 tests are **disabled** due to a deeper pump architecture
issue — NOT a WSL-specific quirk.  Same behavior observed on both WSL and
native Windows (IOCP).

### Root Cause: Pump Task Queue Wake-Up Race

When a PipeStream I/O completes **synchronously** (data already in pipe buffer),
the callback is posted to the pump's task queue via `PostTask`.  The pump
should process this task in its next `DoWork()` cycle.  However, if the pump
has already entered `WaitAndDispatch` (epoll_wait / GetQueuedCompletionStatus)
before the task is posted, there is a race:

- **POSIX**: `PostTask` writes to `eventfd`, which IS in the epoll set.
  epoll_wait should return immediately.  But if the eventfd was already
  drained by `DrainPendingWakeups`, the pump may block indefinitely.
- **Windows**: Similar race with the IOCP wake event.

`pipe_stream_win_test.cpp` works because it uses a **callback chain** — each
callback issues the next ReadAsync, keeping the pump constantly busy with
I/O operations.  Single-shot ReadAsync + wait-for-callback tests expose the
race.

### Investigation Needed

1. Trace `PostTask` → `ScheduleWork` → eventfd write path to verify the
   pump's epoll_wait actually wakes up for tasks posted during idle.
2. Add a stress test: post 1,000 tasks to an idle IO pump and verify all
   execute within a timeout.
3. Consider adding a pump-internal "wake pending" flag that forces an
   immediate DoWork() cycle after any PostTask.

### Disabled Tests

### 1. `PosixYieldQuotaPreventsStarvation`
- **Symptom**: test hangs; reader never receives data or EOF.
- **Root cause**: concurrent writer thread + IO thread reader coordination.
  WSL's epoll_wait scheduling interacts poorly with the pipe buffer
  back-pressure.  On native Linux the writer blocks on write() when the
  pipe buffer is full, the reader drains via epoll, and they proceed in
  lockstep.
- **Fix direction**: verify on native Linux; if it passes, guard with
  `#if !defined(__WSL__)` or a runtime WSL detection helper.

### 2. `WriteToDisconnectedPeerFailsCleanly`
- **Symptom**: test hangs; write callback never fires.
- **Root cause**: WSL pipe implementation allows writing to a pipe whose
  read end is closed (data fits in the 64 KiB kernel buffer) instead of
  returning EPIPE immediately.  On native Linux with `SIGPIPE` ignored,
  `write()` returns -1/EPIPE when the peer is gone.
- **Fix direction**: redesign to use a larger write that exceeds the pipe
  buffer, forcing EPIPE even on WSL; or verify on native Linux.

### 3. `RapidCancelAndRetryStateMachine`
- **Symptom**: test hangs in phase 2; second read never completes.
- **Root cause**: after `stream.reset()` posts `ShutdownAndSelfDestruct`,
  the WSL IO pump does not reliably process the subsequent completion-signal
  task before entering `epoll_wait`, causing a deadlock.
- **Fix direction**: investigate pump task queue ordering on WSL; verify
  the `stream.reset()` + `PostTask(signal)` pattern works on native Linux.

### 4. `RapidWriteCancelAndRetry`
- **Symptom**: same as #3, write direction.
- **Root cause**: same pump task ordering issue as #3.

### Action Plan
1. Run the full test suite on a **native Linux** machine (not WSL).
2. Re-enable any tests that pass natively, guarded by a WSL detection macro.
3. For tests that also fail on native Linux, fix the underlying pump/pipe
   coordination issue.
4. Consider adding a `nei::IsRunningOnWsl()` helper for platform-specific
   test gating.

---

## Pending Optimizations

### POSIX PipeStream — `called_from_pump_` direct dispatch
- **Implemented** in `d01bac7`: when DrainRead/DrainWrite is called from
  the pump watcher callback, the user callback is invoked directly instead
  of going through `BindPostTask→PostTask`.
- **Remaining work**: the continuation path (batch quota exhausted) still
  uses PostTask even though it could theoretically chain directly.  Low
  priority — the current design is correct and the overhead is negligible.

### POSIX PipeStream — epoll oneshot mode
- Consider switching from level-triggered (default) to edge-triggered
  (`EPOLLET`) + oneshot (`EPOLLONESHOT`) for lower wakeup overhead.
  Requires careful re-arming logic in the read/write completion paths.

---

## OnceCallback Templatization — Background & Plan

### Motivation

The current `OnceCallback` in `neixx/functional/callback.h` is a **monomorphic**
`void()` type-erased wrapper:

```cpp
class NEI_API OnceCallback : public CallbackBase {
  void Run() &&;                               // void() only
  template <typename F> OnceCallback(F&&);     // accepts any void() callable
};
```

This is a deliberate simplification vs. Chromium's `base::OnceCallback<R(Args...)>`.
The trade-off:

| Aspect | Current (void()-only) | Target (templated) |
|--------|----------------------|---------------------|
| ABI stability | ✅ Excellent (single class, fixed layout) | ⚠️ Template instantiation per signature |
| Expressiveness | ❌ Cannot type `OnceCallback<void(AddressList)>` | ✅ Full Chromium parity |
| Move-only API | ❌ Forces `std::function` for parameterized callbacks | ✅ `OnceCallback` natively move-only |
| DLL boundary | ✅ Single exported class | ⚠️ Each instantiation in caller's TU |

### Impacted API

- `neixx/net/host_resolver.h`: `ResolveCallback` currently `std::function` due to
  the `void()` limitation. After templatization, it becomes:
  ```cpp
  using ResolveCallback = OnceCallback<void(const AddressList& addresses)>;
  ```
- All `BindOnce` / `BindPostTask` call sites automatically benefit from
  move-only closure semantics.

### Implementation Sketch

1. **Template `OnceCallback<R(Args...)>`**
   - `class OnceCallback` → `template <typename Sig> class OnceCallback`
   - Common type-erased storage (SBO) stays; signature stored in vtable
   - `Run(Args...) &&` dispatches via vtable
   - Provide backward-compat alias: `using OnceClosure = OnceCallback<void()>;`

2. **Update `BindOnce` / `BindPostTask`**
   - `BindOnce(fn, args...)` → `OnceCallback<deduced_signature>`
   - `BindPostTask(runner, cb)` preserves the signature through the trampoline

3. **Update `RepeatingCallback`** similarly for parity

4. **Migrate call sites** — search for `std::function` usages that should be
   `OnceCallback`, replace.

### Dependency

- `neixx/functional/`: callback.h, callback_base.h, callback_internal.h, bind.h,
  bind_post_task.h, cancelable_callback.h
- `neixx/net/`: host_resolver.h (ResolveCallback type alias)
- All modules that use callbacks with parameters

### Risk

- Template explosion (each `R(Args...)` combo = new instantiation)
- ABI surface grows; shared library consumers must agree on instantiation set
- Mitigation: use `extern template` for common signatures in `callback.cpp`

### Decision

**Approve for implementation.** OnceCallback templatization is the next
architectural step toward full Chromium callback parity.  See commit
`6483caa` for the net module that depends on this.

---

## TCP Net Module — Pending Tests

### `TCPServerSocket_FDExhaustion` (POSIX only, P2)

**Goal**: Verify that the server survives transient FD exhaustion (EMFILE/ENFILE)
and recovers when file descriptors become available again.

**Blocked by**: IO thread (MessagePumpForIO) requires epoll FDs internally;
exhausting process-level FDs starves the pump, preventing task dispatch and
deadlocking the test.

**Approach** (drafted, not yet working):
1. Save `RLIMIT_NOFILE`, set a tight soft limit.
2. Exhaust FDs with dummy sockets.
3. Start server (needs 1 listen fd — release a few held FDs first).
4. Connect while exhausted → server `accept4` must handle EMFILE gracefully
   without crashing.
5. Release held FDs → retry connect → must succeed.

**Next steps**:
- Investigate whether the IO pump can be temporarily paused during FD exhaustion.
- Or: test the EMFILE code path directly by calling `accept4` on an fd
  pre-configured to fail, without exhausting system FDs.
- Or: use a mock/epoll-free IO pump for this specific test.
