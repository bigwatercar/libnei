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
