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
