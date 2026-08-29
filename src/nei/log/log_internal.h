#ifndef _NEI_LOG_INTERNAL_H_
#define _NEI_LOG_INTERNAL_H_

#include <nei/log/log.h>

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#if defined(_WIN32)
#include <Windows.h>
#include <DbgHelp.h>
#include <strsafe.h>
#else
#include <errno.h>
#include <execinfo.h>
#include <iconv.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define _NEI_LOG_TLS __declspec(thread)
#else
#define _NEI_LOG_TLS __thread
#endif

static inline int _nei_log_is_flag_char(char c) {
  return (c == '-') || (c == '+') || (c == ' ') || (c == '#') || (c == '0');
}

static inline int _nei_log_is_digit_char(char c) {
  return (c >= '0') && (c <= '9');
}

#if defined(_WIN32)
#define _NEI_LOG_THREAD_YIELD() SwitchToThread()
#else
#define _NEI_LOG_THREAD_YIELD() sched_yield()
#endif

/* ── Atomic primitive helpers (C99 / platform-specific) ──────────────────── *
 * These wrap the minimal operations needed by the lock-free MPSC ring buffer.
 * Windows: Interlocked* intrinsics.  POSIX: GCC/Clang __atomic builtins.    */
#if defined(_WIN32)
typedef volatile LONG _nei_log_atomic32_t;
typedef volatile LONGLONG _nei_log_atomic64_t;
/** Acquire-load a 32-bit value (uses CAS(p,0,0) for acquire semantics). */
#define _NEI_LOG_ATOMIC_LOAD32(p) ((uint32_t)InterlockedCompareExchange((volatile LONG *)(p), 0L, 0L))
/** Release-store a 32-bit value (full barrier via InterlockedExchange). */
#define _NEI_LOG_ATOMIC_STORE32(p, v) (void)InterlockedExchange((volatile LONG *)(p), (LONG)(v))
/** Acquire-load a 64-bit value. */
#define _NEI_LOG_ATOMIC_LOAD64(p) ((uint64_t)InterlockedCompareExchange64((volatile LONGLONG *)(p), 0LL, 0LL))
/** Release-store a 64-bit value. */
#define _NEI_LOG_ATOMIC_STORE64(p, v) (void)InterlockedExchange64((volatile LONGLONG *)(p), (LONGLONG)(v))
/** Atomic compare-and-swap; returns non-zero on success. */
#define _NEI_LOG_ATOMIC_CAS64(p, expected, desired)                                                                    \
  (InterlockedCompareExchange64((volatile LONGLONG *)(p), (LONGLONG)(desired), (LONGLONG)(expected))                   \
   == (LONGLONG)(expected))
/** Atomic fetch-and-add (returns old value, full barrier). */
#define _NEI_LOG_ATOMIC_FETCH_ADD32(p, v) ((uint32_t)InterlockedExchangeAdd((volatile LONG *)(p), (LONG)(v)))
#define _NEI_LOG_ATOMIC_FETCH_SUB32(p, v) ((uint32_t)InterlockedExchangeAdd((volatile LONG *)(p), -(LONG)(v)))
#define _NEI_LOG_ATOMIC_FETCH_ADD64(p, v) ((uint64_t)InterlockedExchangeAdd64((volatile LONGLONG *)(p), (LONGLONG)(v)))
/** Yield CPU hint inside a spin loop. */
#define _NEI_LOG_CPU_YIELD() YieldProcessor()
#else
typedef volatile uint32_t _nei_log_atomic32_t;
typedef volatile uint64_t _nei_log_atomic64_t;
#define _NEI_LOG_ATOMIC_LOAD32(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define _NEI_LOG_ATOMIC_STORE32(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define _NEI_LOG_ATOMIC_FETCH_ADD32(p, v) __atomic_fetch_add((p), (v), __ATOMIC_ACQ_REL)
#define _NEI_LOG_ATOMIC_FETCH_SUB32(p, v) __atomic_fetch_sub((p), (v), __ATOMIC_ACQ_REL)
#define _NEI_LOG_ATOMIC_LOAD64(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define _NEI_LOG_ATOMIC_STORE64(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define _NEI_LOG_ATOMIC_CAS64(p, expected, desired)                                                                    \
  __atomic_compare_exchange_n((p), &(expected), (desired), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define _NEI_LOG_ATOMIC_FETCH_ADD64(p, v) __atomic_fetch_add((p), (v), __ATOMIC_ACQ_REL)
#define _NEI_LOG_CPU_YIELD() sched_yield()
#endif
/* ─────────────────────────────────────────────────────────────────────────── */

/// @brief Sentinel value used for non-verbose logs.
#define _NEI_LOG_NOT_VERBOSE -1

/// @brief Per-record serialization buffer limit in bytes.
#define _NEI_LOG_EVENT_BUFFER_SIZE 8192U

/// @brief Maximum deep-copy size for string arguments in bytes.
#define _NEI_LOG_MAX_STRING_COPY 4096U

/** Maximum inline-copy size for producer-side source-location strings stored
 *  directly in the event header (avoids dangling DLL/SO pointers). */
#define _NEI_LOG_MAX_FILE_COPY 128U
#define _NEI_LOG_MAX_FUNC_COPY 160U
#define _NEI_LOG_MAX_FMT_COPY 112U

/** Number of fixed slots in the MPSC ring buffer.  Each slot holds one full
 *  serialized event (up to _NEI_LOG_EVENT_BUFFER_SIZE bytes).
 *  Default is 256 and can be overridden via CMake option NEI_LOG_RING_SLOTS. */
#if defined(NEI_LOG_RING_SLOTS)
#define _NEI_LOG_RING_SLOTS ((uint32_t)(NEI_LOG_RING_SLOTS))
#else
#define _NEI_LOG_RING_SLOTS 256U
#endif

/** Producer wait strategy thresholds while reserved slot is still occupied.
 *  1) [1, RELAX_ITERS]      : cpu-relax (very short wait)
 *  2) (RELAX_ITERS, YIELD]  : OS thread yield
 *  3) > YIELD_ITERS         : block on condition variable until consumer drains
 */
#define _NEI_LOG_RING_WAIT_RELAX_ITERS 64U
#define _NEI_LOG_RING_WAIT_YIELD_ITERS 2048U
#define _NEI_LOG_FLUSH_SPIN_BASE_ITERS 1024U
#define _NEI_LOG_FLUSH_SPIN_MID_ITERS 2048U
#define _NEI_LOG_FLUSH_SPIN_SHALLOW_ITERS 4096U
#define _NEI_LOG_CONSUMER_IDLE_SPIN_ITERS 512U
#define _NEI_LOG_DEFAULT_FILE_SINK_MAGIC 0x4B4E5346U

/// @brief Compact serialized payload type tags.
enum _nei_log_payload_type_e {
  _NEI_LOG_PAYLOAD_I32 = 1,
  _NEI_LOG_PAYLOAD_U32 = 2,
  _NEI_LOG_PAYLOAD_I64 = 3,
  _NEI_LOG_PAYLOAD_U64 = 4,
  _NEI_LOG_PAYLOAD_DOUBLE = 5,
  _NEI_LOG_PAYLOAD_CHAR = 6,
  _NEI_LOG_PAYLOAD_PTR = 7,
  _NEI_LOG_PAYLOAD_CSTR = 8,
  _NEI_LOG_PAYLOAD_LONGDOUBLE = 9,
  /** Entire user message as length-prefixed bytes; no printf/fmt processing (nei_llog_literal / nei_vlog_literal). */
  _NEI_LOG_PAYLOAD_LITERAL_MSG = 10,
};

#define _NEI_LOG_LONGDOUBLE_STORAGE 16U

/**
 * @brief Header for a compact serialized log record.
 * @details The payload follows immediately after the header; total_size allows fast skipping.
 *
 * Source-location strings (file, func, fmt) are deep-copied into inline buffers
 * at serialization time so the event is self-contained. This eliminates
 * dangling-pointer risks when a DLL/SO is unloaded between emit and consume.
 */
typedef struct _nei_log_event_header_st {
  uint32_t total_size;
  /* natural padding to align timestamp_ns on 8-byte boundary */
  uint64_t timestamp_ns;
  nei_log_config_handle_t config_handle;
  int32_t level;
  int32_t line;
  int32_t verbose;
  /** Length of @ref thread_id_str captured at emit time; @c 0 means omit @c tid= from output. */
  uint8_t thread_id_len;
  /** 1 = literal message (no printf-format string); 0 = printf-style. */
  uint8_t is_literal;
  /** Thread id text (not necessarily '\\0'-terminated; use @ref thread_id_len). Max 23 bytes. */
  char thread_id_str[23];
  /** Padding to align the following char arrays to a 4-byte boundary; absorbs
   *  what would otherwise be trailing padding at the end of the struct. */
  char _reserved[3];
  /** Deep-copied source file path (truncated at _NEI_LOG_MAX_FILE_COPY). */
  char file[_NEI_LOG_MAX_FILE_COPY];
  /** Deep-copied function signature (truncated at _NEI_LOG_MAX_FUNC_COPY). */
  char func[_NEI_LOG_MAX_FUNC_COPY];
  /** Deep-copied printf format string (truncated at _NEI_LOG_MAX_FMT_COPY); empty when @ref is_literal. */
  char fmt[_NEI_LOG_MAX_FMT_COPY];
} nei_log_event_header_st;

/**
 * @brief One slot in the MPSC ring buffer.
 * @details Producers write here lock-free; the consumer reads in order.
 *          `state` is the commit flag: 0 = empty (consumer owns), 1 = committed (data valid).
 *
 * Slot size is padded to a multiple of the cache-line size (64 B) so that
 * adjacent slots never share a cache line, avoiding false sharing between
 * producers writing to different slots. */
#define _NEI_LOG_CACHELINE 64U

typedef struct {
  _nei_log_atomic32_t state;         /**< 0 = empty; 1 = committed. */
  uint32_t size;                     /**< Valid byte count in @ref data. */
  _nei_log_atomic64_t published_seq; /**< Absolute sequence + 1 once this reservation is fully published. */
  uint8_t data[_NEI_LOG_EVENT_BUFFER_SIZE];
  char _pad[_NEI_LOG_CACHELINE
            - ((sizeof(_nei_log_atomic32_t) + sizeof(uint32_t) + sizeof(_nei_log_atomic64_t)
                + _NEI_LOG_EVENT_BUFFER_SIZE)
               % _NEI_LOG_CACHELINE)];
} nei_log_ring_slot_st;

/**
 * @brief MPSC lock-free ring buffer.
 * @details Producers atomically fetch-add `write_pos` to reserve a slot;
 *          the consumer advances `consumer_pos` sequentially.
 *          Both positions are monotonically increasing uint64_t counters;
 *          the actual slot index is `pos % _NEI_LOG_RING_SLOTS`.
 *
 * Each position counter is isolated to its own cache line (64 B padding)
 * to prevent false sharing: `write_pos` is hot-written by all producers,
 * `committed_pos` is CAS'd by producers after publish, and `consumer_pos`
 * is written by the consumer on every drain cycle. */
typedef struct {
  nei_log_ring_slot_st slots[_NEI_LOG_RING_SLOTS];
  _nei_log_atomic64_t write_pos;
  char _pad1[_NEI_LOG_CACHELINE - sizeof(_nei_log_atomic64_t)];
  _nei_log_atomic64_t committed_pos;
  char _pad2[_NEI_LOG_CACHELINE - sizeof(_nei_log_atomic64_t)];
  _nei_log_atomic64_t consumer_pos;
  char _pad3[_NEI_LOG_CACHELINE - sizeof(_nei_log_atomic64_t)];
} nei_log_ring_st;

typedef struct _nei_log_runtime_st {
  nei_log_ring_st ring;

  /* ── Stats: producer-written group (24 B + 40 B pad = 64 B cache line) ── */
  _nei_log_atomic64_t stat_producer_spin_loops;
  _nei_log_atomic64_t stat_producer_spin_total_ns;
  _nei_log_atomic64_t stat_ring_high_watermark;
  char _pad_stats_p[_NEI_LOG_CACHELINE - 3U * sizeof(_nei_log_atomic64_t)];

  /* ── Stats: consumer-written group (48 B + 16 B pad = 64 B cache line) ── */
  _nei_log_atomic64_t stat_consumer_wakeups;
  _nei_log_atomic64_t stat_consumer_drain_total_ns;
  _nei_log_atomic64_t stat_consumer_sync_total_ns;
  _nei_log_atomic64_t stat_consumer_fast_retry_hits;
  _nei_log_atomic64_t stat_consumer_fast_retry_misses;
  _nei_log_atomic64_t stat_consumer_drain_batches;
  char _pad_stats_c[_NEI_LOG_CACHELINE - 6U * sizeof(_nei_log_atomic64_t)];

  /* ── Stats: flush-written (8 B + 56 B pad = 64 B cache line) ── */
  _nei_log_atomic64_t stat_flush_wait_loops;
  char _pad_stats_f[_NEI_LOG_CACHELINE - sizeof(_nei_log_atomic64_t)];

  /* ── consumer_sleeping: hot read by producers, isolated to own cache line ── */
  _nei_log_atomic32_t consumer_sleeping;
  char _pad_cs[_NEI_LOG_CACHELINE - sizeof(_nei_log_atomic32_t)];

  int stop_requested;
  int initialized;
#if defined(_WIN32)
  /** Set by the consumer thread on entry; used to make @ref nei_log_flush a no-op on that thread. */
  DWORD consumer_thread_id;
  CRITICAL_SECTION mutex;
  CONDITION_VARIABLE cond;
  HANDLE thread;
#else
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  pthread_t thread;
#endif
  uint32_t auto_flush_interval_ms; /**< Consumer idle-wake interval (ms), 0=disabled. */
} nei_log_runtime_st;

typedef struct _nei_log_default_file_sink_ctx_st {
  uint32_t magic;
  FILE *fp;
  uint32_t flush_counter;  /* Batch flush: count logs since last fflush. */
  uint32_t flush_interval; /* Flush after this many logs (0=disable batching, always fflush). */
  uint32_t file_buffer_bytes;
  char *filename;
  uint32_t max_backup_files;
  char *write_batch_buf;
  size_t current_size;
  size_t max_file_bytes;
  size_t write_batch_cap;
  size_t write_batch_used;
} nei_log_default_file_sink_ctx_st;

#define _NEI_LOG_MAX_CONFIGS 16U // includes default config

/* Global variables - defined in log_config.c.
 * s_custom_configs holds the in-place storage for slot 0 (the default
 * config, which callers may mutate in place via nei_log_default_config).
 * Slots 1+ are published as immutable malloc'd copies by
 * nei_log_add_config (replacement, never in-place rewrites — see there). */
extern nei_log_config_st *s_config_ptrs[_NEI_LOG_MAX_CONFIGS];
extern nei_log_config_st s_custom_configs[_NEI_LOG_MAX_CONFIGS];
extern uint8_t s_config_used[_NEI_LOG_MAX_CONFIGS];
extern _nei_log_atomic32_t s_config_active_emit_counts[_NEI_LOG_MAX_CONFIGS];
extern _nei_log_atomic32_t s_config_table_initialized;
#if defined(_WIN32)
extern volatile LONGLONG s_config_snapshot;
#else
extern uint64_t s_config_snapshot;
#endif
#if defined(_WIN32)
extern SRWLOCK s_config_lock;
#define _NEI_LOG_SIGNAL_COND(cv) WakeConditionVariable(cv)
#define _NEI_LOG_BROADCAST_COND(cv) WakeAllConditionVariable(cv)
#else
extern pthread_rwlock_t s_config_lock;
#define _NEI_LOG_SIGNAL_COND(cv) pthread_cond_signal(cv)
#define _NEI_LOG_BROADCAST_COND(cv) pthread_cond_broadcast(cv)
#endif

/* Global variables - defined in log_runtime.c */
extern nei_log_runtime_st s_runtime;

/* Log level string tables - defined in log_format.c */
extern const char *s_level_strings[];
extern const char *s_level_short_strings[];

/* Function declarations used across modules */

/* From log_config.c */
void _nei_log_ensure_config_table_initialized(void);
void _nei_log_fill_default_config(nei_log_config_st *cfg);
void _nei_log_reset_default_config(void);
void _nei_log_config_lock_read(void);
void _nei_log_config_lock_write(void);
void _nei_log_config_unlock_read(void);
void _nei_log_config_unlock_write(void);
nei_log_config_handle_t _nei_log_make_handle_from_slot(size_t slot);
int _nei_log_slot_from_handle(nei_log_config_handle_t handle, size_t *out_slot);
uint64_t _nei_log_config_snapshot_load(void);
void _nei_log_config_snapshot_bump(void);
const nei_log_config_st *_nei_log_acquire_config_for_emit(nei_log_config_handle_t handle, size_t *out_slot);
void _nei_log_release_config_after_emit(size_t slot);
void _nei_log_wait_for_emit_quiescent(size_t slot);

/* From log_runtime.c */
int _nei_log_ensure_runtime_initialized(void);
void _nei_log_shutdown_runtime(void);
void _nei_log_auto_flush_file_sinks(void);
int _nei_log_enqueue_event(const uint8_t *event, size_t len);
void _nei_log_signal_consumer_if_sleeping(void);
uint32_t _nei_log_get_runtime_init_count_for_test(void);
int _nei_log_get_perf_stats_for_test(nei_log_perf_stats_st *out_stats);
void _nei_log_reset_perf_stats_for_test(void);
int _nei_log_reserve_unpublished_slot_for_test(uint64_t *out_reserved_pos);
int _nei_log_rollback_unpublished_slot_for_test(uint64_t reserved_pos);

/* From log_thread_id.c */
void _nei_log_tls_thread_id_cstr(const char **out_str, size_t *out_len);
int _nei_log_config_wants_thread_id(nei_log_config_handle_t config_handle);
void _nei_log_header_fill_thread_id(nei_log_event_header_st *header, nei_log_config_handle_t config_handle);

/* From log_serialize.c */
size_t _nei_log_serialize_event(uint8_t *out,
                                size_t out_cap,
                                nei_log_config_handle_t config_handle,
                                const char *file,
                                int32_t line,
                                const char *func,
                                int32_t level,
                                int32_t verbose,
                                const char *fmt,
                                va_list args);
size_t _nei_log_serialize_literal_msg(uint8_t *out,
                                      size_t out_cap,
                                      nei_log_config_handle_t config_handle,
                                      const char *file,
                                      int32_t line,
                                      const char *func,
                                      int32_t level,
                                      int32_t verbose,
                                      const char *message,
                                      size_t message_length);

/* From log_format.c */
const char *_get_level_string(nei_log_level_e level);
const char *_get_level_short_string(nei_log_level_e level);
void _nei_log_format_timestamp(uint64_t timestamp_ns, nei_log_timestamp_style_e style, char *out, size_t out_size);
int _nei_log_format_event(const nei_log_event_header_st *header,
                          const nei_log_config_st *effective_config,
                          const uint8_t *payload,
                          size_t payload_size,
                          char *out,
                          size_t out_cap);

/* From log_sink.c */
void _nei_log_default_file_llog(const nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length);
void _nei_log_default_file_vlog(const nei_log_sink_st *sink, int verbose, const char *message, size_t length);
void _nei_log_emit_message(
    const nei_log_config_st *config, int32_t level, int32_t verbose, const char *message, size_t length);

/* From log_serialize.c */
uint64_t _nei_log_now_ns(void);

/* Crash handler */
void _nei_log_immediate_crash(void);

#endif /* _NEI_LOG_INTERNAL_H_ */
