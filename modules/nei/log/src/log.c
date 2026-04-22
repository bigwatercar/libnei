#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <nei/log/log.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>
#include <wchar.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <pthread.h>
#include <errno.h>
#include <iconv.h>
#endif

#if defined(_WIN32)
#define _NEI_LOG_TLS __declspec(thread)
#else
#define _NEI_LOG_TLS __thread
#endif

/// @brief Sentinel value used for non-verbose logs.
#define _NEI_LOG_NOT_VERBOSE -1

/// @brief Per-record serialization buffer limit in bytes.
#define _NEI_LOG_EVENT_BUFFER_SIZE 8192U

/// @brief Maximum deep-copy size for string arguments in bytes.
#define _NEI_LOG_MAX_STRING_COPY 4096U

/// @brief Capacity of each buffer in the global double-buffer, in bytes.
#define _NEI_LOG_GLOBAL_BUFFER_CAPACITY (1024U * 1024U)
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
 */
typedef struct _nei_log_event_header_st {
  uint32_t total_size;
  uint64_t timestamp_ns;
  nei_log_config_handle_t config_handle;
  const char *file_ptr;
  const char *func_ptr;
  const char *fmt_ptr;
  int32_t level;
  int32_t line;
  int32_t verbose;
  /** Length of @ref thread_id_str captured at emit time; @c 0 means omit @c tid= from output. */
  uint8_t thread_id_len;
  /** Thread id text (not necessarily '\\0'-terminated; use @ref thread_id_len). Max 23 bytes. */
  char thread_id_str[23];
} nei_log_event_header_st;

typedef struct _nei_log_runtime_st {
  uint8_t buffer_a[_NEI_LOG_GLOBAL_BUFFER_CAPACITY];
  uint8_t buffer_b[_NEI_LOG_GLOBAL_BUFFER_CAPACITY];
  size_t used[2];
  int active_index;
  int pending_index;
  /** Buffer index currently being read by the consumer (-1 if none). */
  int consuming_index;
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
} nei_log_runtime_st;

static nei_log_runtime_st s_runtime = {
    .used = {0U, 0U},
    .active_index = 0,
    .pending_index = -1,
    .consuming_index = -1,
    .stop_requested = 0,
    .initialized = 0,
};

typedef struct _nei_log_default_file_sink_ctx_st {
  uint32_t magic;
  FILE *fp;
} nei_log_default_file_sink_ctx_st;

// NOTE: This config table is intended to be managed during initialization and
// is not designed for concurrent add/remove while logging is active.
#define _NEI_LOG_MAX_CONFIGS 16U // includes default config

static nei_log_config_st *s_config_ptrs[_NEI_LOG_MAX_CONFIGS];
static nei_log_config_st s_custom_configs[_NEI_LOG_MAX_CONFIGS];
static uint8_t s_config_used[_NEI_LOG_MAX_CONFIGS];
static int s_config_table_initialized = 0;
#if defined(_WIN32)
static SRWLOCK s_config_lock = SRWLOCK_INIT;
#else
static pthread_rwlock_t s_config_lock = PTHREAD_RWLOCK_INITIALIZER;
#endif

#pragma region level string tables

// clang-format off
#define _NEI_LOG_LVL_TAGS \
  _NEI_LOG_LVL_STR_PAIRS("VERBOSE", "V"), \
  _NEI_LOG_LVL_STR_PAIRS("TRACE",   "T"), \
  _NEI_LOG_LVL_STR_PAIRS("DEBUG",   "D"), \
  _NEI_LOG_LVL_STR_PAIRS("INFO",    "I"), \
  _NEI_LOG_LVL_STR_PAIRS("WARN",    "W"), \
  _NEI_LOG_LVL_STR_PAIRS("ERROR",   "E"), \
  _NEI_LOG_LVL_STR_PAIRS("FATAL",   "F")
// clang-format on
#define _NEI_LOG_LVL_STR_PAIRS(_longstr, _shortstr) _longstr
/** @brief Full log level tag table. */
static const char *s_level_strings[] = {_NEI_LOG_LVL_TAGS};
#undef _NEI_LOG_LVL_STR_PAIRS
#define _NEI_LOG_LVL_STR_PAIRS(_longstr, _shortstr) _shortstr
/** @brief Short log level tag table. */
static const char *s_level_short_strings[] = {_NEI_LOG_LVL_TAGS};
#undef _NEI_LOG_LVL_STR_PAIRS
#undef _NEI_LOG_LVL_TAGS

/** @brief Get the full log level tag. */
static inline const char *_get_level_string(nei_log_level_e level) {
  assert(level >= NEI_L_VERBOSE && level <= NEI_L_FATAL);
  return s_level_strings[level];
}

/** @brief Get the short log level tag. */
static inline const char *_get_level_short_string(nei_log_level_e level) {
  assert(level >= NEI_L_VERBOSE && level <= NEI_L_FATAL);
  return s_level_short_strings[level];
}

#pragma endregion

#pragma region static prototypes (late-defined helpers)

/* Config table */
static void _nei_log_ensure_config_table_initialized(void);
static void _nei_log_fill_default_config(nei_log_config_st *cfg);
static void _nei_log_reset_default_config(void);
static void _nei_log_config_lock_read(void);
static void _nei_log_config_lock_write(void);
static void _nei_log_config_unlock_read(void);
static void _nei_log_config_unlock_write(void);
static nei_log_config_handle_t _nei_log_make_handle_from_slot(size_t slot);
static int _nei_log_slot_from_handle(nei_log_config_handle_t handle, size_t *out_slot);

/* Sink */
static void
_nei_log_default_file_llog(const nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length);
static void _nei_log_default_file_vlog(const nei_log_sink_st *sink, int verbose, const char *message, size_t length);

/* Serialization + runtime (called from public API / flush before these definitions appear). */
static size_t _nei_log_serialize_event(uint8_t *out,
                                       size_t out_cap,
                                       nei_log_config_handle_t config_handle,
                                       const char *file,
                                       int32_t line,
                                       const char *func,
                                       int32_t level,
                                       int32_t verbose,
                                       const char *fmt,
                                       va_list args);
static size_t _nei_log_serialize_literal_msg(uint8_t *out,
                                             size_t out_cap,
                                             nei_log_config_handle_t config_handle,
                                             const char *file,
                                             int32_t line,
                                             const char *func,
                                             int32_t level,
                                             int32_t verbose,
                                             const char *message,
                                             size_t message_length);
static int _nei_log_ensure_runtime_initialized(void);
static void _nei_log_shutdown_runtime(void);
static int _nei_log_enqueue_event(const uint8_t *event, size_t len);
static void _nei_log_ensure_active_not_consuming(nei_log_runtime_st *rt);
#if defined(_WIN32)
static DWORD WINAPI _nei_log_consumer_thread(LPVOID arg);
#else
static void *_nei_log_consumer_thread(void *arg);
#endif

#pragma endregion

#pragma region thread id (TLS, producer)

#if defined(_WIN32)
static _NEI_LOG_TLS char s_tls_tid_buf[32];
static _NEI_LOG_TLS DWORD s_tls_tid_dw;
static _NEI_LOG_TLS unsigned char s_tls_tid_ready;
#else
static _NEI_LOG_TLS char s_tls_tid_buf[32];
static _NEI_LOG_TLS pthread_t s_tls_tid_pt;
static _NEI_LOG_TLS unsigned char s_tls_tid_ready;
#endif

static void _nei_log_tls_thread_id_cstr(const char **out_str, size_t *out_len) {
#if defined(_WIN32)
  const DWORD id = GetCurrentThreadId();
  if (s_tls_tid_ready == 0U || s_tls_tid_dw != id) {
    (void)snprintf(s_tls_tid_buf, sizeof(s_tls_tid_buf), "%lu", (unsigned long)id);
    s_tls_tid_dw = id;
    s_tls_tid_ready = 1U;
  }
#else
  const pthread_t self = pthread_self();
  if (s_tls_tid_ready == 0U || pthread_equal(s_tls_tid_pt, self) == 0) {
    (void)snprintf(s_tls_tid_buf, sizeof(s_tls_tid_buf), "%lu", (unsigned long)self);
    s_tls_tid_pt = self;
    s_tls_tid_ready = 1U;
  }
#endif
  if (out_str != NULL) {
    *out_str = s_tls_tid_buf;
  }
  if (out_len != NULL) {
    *out_len = strlen(s_tls_tid_buf);
  }
}

static int _nei_log_config_wants_thread_id(nei_log_config_handle_t config_handle) {
  size_t slot = 0U;
  int want = 0;
  _nei_log_config_lock_read();
  _nei_log_ensure_config_table_initialized();
  if (_nei_log_slot_from_handle(config_handle, &slot) == 0 && s_config_used[slot] != 0U) {
    const nei_log_config_st *cfg = s_config_ptrs[slot];
    if (cfg != NULL && cfg->log_thread_id != 0) {
      want = 1;
    }
  }
  _nei_log_config_unlock_read();
  return want;
}

static void _nei_log_header_fill_thread_id(nei_log_event_header_st *header, nei_log_config_handle_t config_handle) {
  const char *tid_str = NULL;
  size_t tid_len = 0U;
  if (header == NULL) {
    return;
  }
  header->thread_id_len = 0;
  if (!_nei_log_config_wants_thread_id(config_handle)) {
    return;
  }
  _nei_log_tls_thread_id_cstr(&tid_str, &tid_len);
  if (tid_str == NULL || tid_len == 0U) {
    return;
  }
  if (tid_len > sizeof(header->thread_id_str)) {
    tid_len = sizeof(header->thread_id_str);
  }
  memcpy(header->thread_id_str, tid_str, tid_len);
  header->thread_id_len = (uint8_t)tid_len;
}

#pragma endregion

#pragma region public API

int nei_log_add_config(const nei_log_config_st *config, nei_log_config_handle_t *out_handle) {
  if (config == NULL) {
    return -1;
  }

  _nei_log_config_lock_write();
  _nei_log_ensure_config_table_initialized();

  // Find a free slot for new config (slot 0 is reserved for default).
  size_t free_slot = (size_t)-1;
  for (size_t slot = 1U; slot < _NEI_LOG_MAX_CONFIGS; ++slot) {
    if (s_config_used[slot] == 0U) {
      free_slot = slot;
      break;
    }
  }
  if (free_slot == (size_t)-1) {
    _nei_log_config_unlock_write();
    return -1;
  }

  s_config_used[free_slot] = 1U;
  memcpy(&s_custom_configs[free_slot], config, sizeof(*config));
  s_config_ptrs[free_slot] = &s_custom_configs[free_slot];
  if (out_handle != NULL) {
    *out_handle = _nei_log_make_handle_from_slot(free_slot);
  }

  _nei_log_config_unlock_write();
  return 0;
}

void nei_log_remove_config(nei_log_config_handle_t handle) {
  size_t slot = 0U;
  _nei_log_config_lock_write();
  _nei_log_ensure_config_table_initialized();
  if (_nei_log_slot_from_handle(handle, &slot) != 0 || s_config_used[slot] == 0U) {
    _nei_log_config_unlock_write();
    return;
  }
  if (slot == 0U) {
    _nei_log_reset_default_config();
    _nei_log_config_unlock_write();
    return;
  }
  s_config_used[slot] = 0U;
  s_config_ptrs[slot] = NULL;
  _nei_log_config_unlock_write();
}

nei_log_config_st *nei_log_get_config(nei_log_config_handle_t handle) {
  nei_log_config_st *cfg = NULL;
  size_t slot = 0U;
  _nei_log_config_lock_read();
  _nei_log_ensure_config_table_initialized();
  if (_nei_log_slot_from_handle(handle, &slot) != 0 || s_config_used[slot] == 0U) {
    _nei_log_config_unlock_read();
    return NULL;
  }
  cfg = s_config_ptrs[slot];
  _nei_log_config_unlock_read();
  return cfg;
}

nei_log_config_st *nei_log_default_config(void) {
  nei_log_config_st *cfg = NULL;
  _nei_log_config_lock_write();
  _nei_log_ensure_config_table_initialized();
  cfg = s_config_ptrs[0];
  if (cfg == NULL) {
    s_config_used[0] = 1U;
    s_config_ptrs[0] = &s_custom_configs[0];
    cfg = s_config_ptrs[0];
    _nei_log_fill_default_config(cfg);
  }
  _nei_log_config_unlock_write();
  return cfg;
}

nei_log_sink_st *nei_log_create_default_file_sink(const char *filename) {
  nei_log_sink_st *sink = NULL;
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  FILE *fp = NULL;

  if (filename == NULL || filename[0] == '\0') {
    return NULL;
  }

#if defined(_WIN32)
  if (fopen_s(&fp, filename, "ab") != 0) {
    fp = NULL;
  }
#else
  fp = fopen(filename, "ab");
#endif
  if (fp == NULL) {
    return NULL;
  }

  sink = (nei_log_sink_st *)calloc(1U, sizeof(*sink));
  ctx = (nei_log_default_file_sink_ctx_st *)calloc(1U, sizeof(*ctx));
  if (sink == NULL || ctx == NULL) {
    if (fp != NULL) {
      fclose(fp);
    }
    free(ctx);
    free(sink);
    return NULL;
  }

  ctx->magic = _NEI_LOG_DEFAULT_FILE_SINK_MAGIC;
  ctx->fp = fp;

  sink->llog = _nei_log_default_file_llog;
  sink->vlog = _nei_log_default_file_vlog;
  sink->opaque = ctx;
  return sink;
}

void nei_log_destroy_sink(nei_log_sink_st *sink) {
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  if (sink == NULL) {
    return;
  }

  if (sink->opaque != NULL && sink->llog == _nei_log_default_file_llog && sink->vlog == _nei_log_default_file_vlog) {
    ctx = (nei_log_default_file_sink_ctx_st *)sink->opaque;
    if (ctx->magic == _NEI_LOG_DEFAULT_FILE_SINK_MAGIC) {
      if (ctx->fp != NULL) {
        fclose(ctx->fp);
      }
      ctx->fp = NULL;
      ctx->magic = 0U;
      free(ctx);
    }
  }
  free(sink);
}

void nei_llog(nei_log_config_handle_t config_handle,
              nei_log_level_e level,
              const char *file,
              int32_t line,
              const char *func,
              const char *fmt,
              ...) {
  va_list args;
  va_list scan_args;
  uint8_t event[_NEI_LOG_EVENT_BUFFER_SIZE];

  (void)_nei_log_ensure_runtime_initialized();
  va_start(args, fmt);
  va_copy(scan_args, args);
  {
    const size_t serialized_len = _nei_log_serialize_event(
        event, sizeof(event), config_handle, file, line, func, (int32_t)level, _NEI_LOG_NOT_VERBOSE, fmt, scan_args);
    if (serialized_len > 0U) {
      (void)_nei_log_enqueue_event(event, serialized_len);
    }
  }
  va_end(scan_args);
  va_end(args);
}

void nei_vlog(nei_log_config_handle_t config_handle,
              int verbose,
              const char *file,
              int32_t line,
              const char *func,
              const char *fmt,
              ...) {
  va_list args;
  va_list scan_args;
  uint8_t event[_NEI_LOG_EVENT_BUFFER_SIZE];

  (void)_nei_log_ensure_runtime_initialized();
  va_start(args, fmt);
  va_copy(scan_args, args);
  {
    const size_t serialized_len = _nei_log_serialize_event(event,
                                                           sizeof(event),
                                                           config_handle,
                                                           file,
                                                           line,
                                                           func,
                                                           (int32_t)NEI_L_VERBOSE,
                                                           (int32_t)verbose,
                                                           fmt,
                                                           scan_args);
    if (serialized_len > 0U) {
      (void)_nei_log_enqueue_event(event, serialized_len);
    }
  }
  va_end(scan_args);
  va_end(args);
}

void nei_llog_literal(nei_log_config_handle_t config_handle,
                      nei_log_level_e level,
                      const char *file,
                      int32_t line,
                      const char *func,
                      const char *message,
                      size_t length) {
  uint8_t event[_NEI_LOG_EVENT_BUFFER_SIZE];

  (void)_nei_log_ensure_runtime_initialized();
  {
    const size_t serialized_len = _nei_log_serialize_literal_msg(
        event, sizeof(event), config_handle, file, line, func, (int32_t)level, _NEI_LOG_NOT_VERBOSE, message, length);
    if (serialized_len > 0U) {
      (void)_nei_log_enqueue_event(event, serialized_len);
    }
  }
}

void nei_vlog_literal(nei_log_config_handle_t config_handle,
                      int verbose,
                      const char *file,
                      int32_t line,
                      const char *func,
                      const char *message,
                      size_t length) {
  uint8_t event[_NEI_LOG_EVENT_BUFFER_SIZE];

  (void)_nei_log_ensure_runtime_initialized();
  {
    const size_t serialized_len = _nei_log_serialize_literal_msg(event,
                                                                 sizeof(event),
                                                                 config_handle,
                                                                 file,
                                                                 line,
                                                                 func,
                                                                 (int32_t)NEI_L_VERBOSE,
                                                                 (int32_t)verbose,
                                                                 message,
                                                                 length);
    if (serialized_len > 0U) {
      (void)_nei_log_enqueue_event(event, serialized_len);
    }
  }
}

static int _nei_log_is_consumer_thread(void) {
  if (!s_runtime.initialized) {
    return 0;
  }
#if defined(_WIN32)
  return s_runtime.consumer_thread_id != 0U && GetCurrentThreadId() == s_runtime.consumer_thread_id;
#else
  return pthread_equal(pthread_self(), s_runtime.thread) != 0;
#endif
}

void nei_log_flush(void) {
  if (!s_runtime.initialized) {
    return;
  }
  if (_nei_log_is_consumer_thread()) {
    return;
  }

#if defined(_WIN32)
  EnterCriticalSection(&s_runtime.mutex);
  for (;;) {
    if (s_runtime.pending_index == -1 && s_runtime.used[s_runtime.active_index] > 0U) {
      const int active = s_runtime.active_index;
      s_runtime.pending_index = active;
      s_runtime.active_index = 1 - active;
      _nei_log_ensure_active_not_consuming(&s_runtime);
      WakeAllConditionVariable(&s_runtime.cond);
    }
    if (s_runtime.pending_index == -1 && s_runtime.consuming_index == -1
        && s_runtime.used[s_runtime.active_index] == 0U) {
      break;
    }
    SleepConditionVariableCS(&s_runtime.cond, &s_runtime.mutex, INFINITE);
  }
  LeaveCriticalSection(&s_runtime.mutex);
#else
  pthread_mutex_lock(&s_runtime.mutex);
  for (;;) {
    if (s_runtime.pending_index == -1 && s_runtime.used[s_runtime.active_index] > 0U) {
      const int active = s_runtime.active_index;
      s_runtime.pending_index = active;
      s_runtime.active_index = 1 - active;
      _nei_log_ensure_active_not_consuming(&s_runtime);
      pthread_cond_broadcast(&s_runtime.cond);
    }
    if (s_runtime.pending_index == -1 && s_runtime.consuming_index == -1
        && s_runtime.used[s_runtime.active_index] == 0U) {
      break;
    }
    pthread_cond_wait(&s_runtime.cond, &s_runtime.mutex);
  }
  pthread_mutex_unlock(&s_runtime.mutex);
#endif
}

#pragma endregion

#pragma region log config table implementation

static void _nei_log_ensure_config_table_initialized(void) {
  if (s_config_table_initialized) {
    return;
  }

  memset(s_config_ptrs, 0, sizeof(s_config_ptrs));
  memset(s_custom_configs, 0, sizeof(s_custom_configs));
  memset(s_config_used, 0, sizeof(s_config_used));

  // Slot 0 is the default config.
  s_config_used[0] = 1U;
  _nei_log_fill_default_config(&s_custom_configs[0]);
  s_config_ptrs[0] = &s_custom_configs[0];

  s_config_table_initialized = 1;
}

static void _nei_log_fill_default_config(nei_log_config_st *cfg) {
  if (cfg == NULL) {
    return;
  }
  // Keep sinks NULL by clearing the whole structure.
  memset(cfg, 0, sizeof(*cfg));
  cfg->level_flags.all = 0xFFFFFFFFu;
  cfg->verbose_threshold = -1;
  cfg->short_level_tag = 1;
  cfg->short_path = 1;
  cfg->log_location = 1;
  cfg->log_location_after_message = 1;
  cfg->log_thread_id = 1;
  cfg->log_to_console = 0;
  cfg->timestamp_style = NEI_LOG_TIMESTAMP_STYLE_DEFAULT;
}

static void _nei_log_reset_default_config(void) {
  _nei_log_fill_default_config(&s_custom_configs[0]);
}

static void _nei_log_config_lock_read(void) {
#if defined(_WIN32)
  AcquireSRWLockShared(&s_config_lock);
#else
  (void)pthread_rwlock_rdlock(&s_config_lock);
#endif
}

static void _nei_log_config_lock_write(void) {
#if defined(_WIN32)
  AcquireSRWLockExclusive(&s_config_lock);
#else
  (void)pthread_rwlock_wrlock(&s_config_lock);
#endif
}

static void _nei_log_config_unlock_read(void) {
#if defined(_WIN32)
  ReleaseSRWLockShared(&s_config_lock);
#else
  (void)pthread_rwlock_unlock(&s_config_lock);
#endif
}

static void _nei_log_config_unlock_write(void) {
#if defined(_WIN32)
  ReleaseSRWLockExclusive(&s_config_lock);
#else
  (void)pthread_rwlock_unlock(&s_config_lock);
#endif
}

static nei_log_config_handle_t _nei_log_make_handle_from_slot(size_t slot) {
  if (slot >= _NEI_LOG_MAX_CONFIGS) {
    return NEI_LOG_INVALID_CONFIG_HANDLE;
  }
  return (nei_log_config_handle_t)(slot + 1U);
}

static int _nei_log_slot_from_handle(nei_log_config_handle_t handle, size_t *out_slot) {
  size_t slot = 0U;
  if (out_slot == NULL || handle == NEI_LOG_INVALID_CONFIG_HANDLE) {
    return -1;
  }
  slot = (size_t)(handle - (nei_log_config_handle_t)1U);
  if (slot >= _NEI_LOG_MAX_CONFIGS) {
    return -1;
  }
  *out_slot = slot;
  return 0;
}

#pragma endregion

#pragma region log sink implementation

static void
_nei_log_default_file_llog(const nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length) {
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  (void)level;
  if (sink == NULL || message == NULL) {
    return;
  }
  ctx = (nei_log_default_file_sink_ctx_st *)sink->opaque;
  if (ctx == NULL || ctx->magic != _NEI_LOG_DEFAULT_FILE_SINK_MAGIC || ctx->fp == NULL) {
    return;
  }
  (void)fwrite(message, 1U, length, ctx->fp);
  (void)fputc('\n', ctx->fp);
  (void)fflush(ctx->fp);
}

static void _nei_log_default_file_vlog(const nei_log_sink_st *sink, int verbose, const char *message, size_t length) {
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  if (sink == NULL || message == NULL) {
    return;
  }
  ctx = (nei_log_default_file_sink_ctx_st *)sink->opaque;
  if (ctx == NULL || ctx->magic != _NEI_LOG_DEFAULT_FILE_SINK_MAGIC || ctx->fp == NULL) {
    return;
  }
  (void)verbose;
  (void)fwrite(message, 1U, length, ctx->fp);
  (void)fputc('\n', ctx->fp);
  (void)fflush(ctx->fp);
}

#pragma endregion

#pragma region log core (time)

#if defined(_WIN32)
typedef VOID(WINAPI *_nei_pfn_GetSystemTimePreciseAsFileTime)(LPFILETIME);

static INIT_ONCE s_nei_log_win_time_once = INIT_ONCE_STATIC_INIT;
static _nei_pfn_GetSystemTimePreciseAsFileTime s_nei_log_pfn_GetSystemTimePreciseAsFileTime;

static BOOL CALLBACK _nei_log_win_time_init_once(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
  HMODULE k32;
  (void)init_once;
  (void)parameter;
  (void)context;
  k32 = GetModuleHandleW(L"kernel32.dll");
  if (k32 != NULL) {
    s_nei_log_pfn_GetSystemTimePreciseAsFileTime =
        (_nei_pfn_GetSystemTimePreciseAsFileTime)(void *)GetProcAddress(k32, "GetSystemTimePreciseAsFileTime");
  }
  return TRUE;
}

static void _nei_log_win32_get_system_time_as_filetime(LPFILETIME ft) {
  (void)InitOnceExecuteOnce(&s_nei_log_win_time_once, _nei_log_win_time_init_once, NULL, NULL);
  if (s_nei_log_pfn_GetSystemTimePreciseAsFileTime != NULL) {
    s_nei_log_pfn_GetSystemTimePreciseAsFileTime(ft);
  } else {
    GetSystemTimeAsFileTime(ft);
  }
}
#endif

static uint64_t _nei_log_now_ns(void) {
#if defined(_WIN32)
  FILETIME ft;
  ULARGE_INTEGER uli;
  const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
  _nei_log_win32_get_system_time_as_filetime(&ft);
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  if (uli.QuadPart <= EPOCH_DIFF_100NS) {
    return 0;
  }
  return (uli.QuadPart - EPOCH_DIFF_100NS) * 100ULL;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 0;
  }
  return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
#endif
}

#pragma endregion /* log core (time) */

#pragma region log core (serialization)

static inline size_t _nei_log_align_up_8(size_t n) {
  return (n + 7U) & ~(size_t)7U;
}

static int _nei_log_is_flag_char(char c) {
  return (c == '-') || (c == '+') || (c == ' ') || (c == '#') || (c == '0');
}

static int _nei_log_is_digit_char(char c) {
  return (c >= '0') && (c <= '9');
}

static int _nei_log_payload_write_u8(uint8_t *out, size_t out_cap, size_t *used, uint8_t value) {
  if (out == NULL || used == NULL || *used + 1U > out_cap) {
    return -1;
  }
  out[*used] = value;
  *used += 1U;
  return 0;
}

static int _nei_log_payload_write_u16(uint8_t *out, size_t out_cap, size_t *used, uint16_t value) {
  if (out == NULL || used == NULL || *used + sizeof(uint16_t) > out_cap) {
    return -1;
  }
  memcpy(out + *used, &value, sizeof(uint16_t));
  *used += sizeof(uint16_t);
  return 0;
}

static int _nei_log_payload_write_bytes(uint8_t *out, size_t out_cap, size_t *used, const void *src, size_t len) {
  if (out == NULL || used == NULL || src == NULL || *used + len > out_cap) {
    return -1;
  }
  memcpy(out + *used, src, len);
  *used += len;
  return 0;
}

static int _nei_log_payload_write_padded_zero(uint8_t *out, size_t out_cap, size_t *used, size_t target_size) {
  if (out == NULL || used == NULL || target_size < *used || target_size > out_cap) {
    return -1;
  }
  while (*used < target_size) {
    out[*used] = 0;
    *used += 1U;
  }
  return 0;
}

/** Windows: ACP (ANSI code page). POSIX: UTF-8 via iconv from WCHAR_T. */
static const char *_nei_log_wstr_to_mbs_or_placeholder(const wchar_t *ws, char *buf, size_t buf_cap) {
  static const char s_null_placeholder[] = "(null)";
  static const char s_encoding_error_placeholder[] = "[encoding error]";

  if (ws == NULL) {
    return s_null_placeholder;
  }
  if (buf == NULL || buf_cap == 0U) {
    return s_encoding_error_placeholder;
  }

  buf[0] = '\0';
  if (*ws == L'\0') {
    return buf;
  }

#if defined(_WIN32)
  size_t wlen = 0U;
  size_t lo;
  size_t hi;
  size_t best = 0U;
  int out_bytes = 0;

  while (ws[wlen] != L'\0') {
    ++wlen;
  }
  if (wlen > (size_t)INT_MAX) {
    return s_encoding_error_placeholder;
  }

  /* Use system ANSI code page (ACP), not UTF-8, so log text matches typical console/file encodings. */
  out_bytes = WideCharToMultiByte(CP_ACP, 0, ws, (int)wlen, NULL, 0, NULL, NULL);
  if (out_bytes > 0 && (size_t)out_bytes <= (buf_cap - 1U)) {
    int written = WideCharToMultiByte(CP_ACP, 0, ws, (int)wlen, buf, (int)(buf_cap - 1U), NULL, NULL);
    if (written <= 0) {
      return s_encoding_error_placeholder;
    }
    buf[written] = '\0';
    return buf;
  }

  lo = 0U;
  hi = wlen;
  while (lo <= hi) {
    size_t mid = lo + (hi - lo) / 2U;
    int need = 0;
    if (mid > 0U) {
      if (mid > (size_t)INT_MAX) {
        need = -1;
      } else {
        need = WideCharToMultiByte(CP_ACP, 0, ws, (int)mid, NULL, 0, NULL, NULL);
      }
    }
    if (need >= 0 && (size_t)need <= (buf_cap - 1U)) {
      best = mid;
      lo = mid + 1U;
    } else {
      if (mid == 0U) {
        break;
      }
      hi = mid - 1U;
    }
  }
  if (best == 0U) {
    return s_encoding_error_placeholder;
  }
  if (best > (size_t)INT_MAX) {
    return s_encoding_error_placeholder;
  }
  out_bytes = WideCharToMultiByte(CP_ACP, 0, ws, (int)best, buf, (int)(buf_cap - 1U), NULL, NULL);
  if (out_bytes <= 0) {
    return s_encoding_error_placeholder;
  }
  buf[out_bytes] = '\0';
  return buf;
#else
  size_t wlen = 0U;
  iconv_t cd;
  size_t in_left;
  size_t out_left;
  char *in_ptr;
  char *out_ptr;

  while (ws[wlen] != L'\0') {
    ++wlen;
  }
  if (wlen == 0U) {
    return buf;
  }
  if (wlen > (SIZE_MAX / sizeof(wchar_t))) {
    return s_encoding_error_placeholder;
  }

  cd = iconv_open("UTF-8", "WCHAR_T");
  if (cd == (iconv_t)-1) {
    return s_encoding_error_placeholder;
  }

  in_left = wlen * sizeof(wchar_t);
  out_left = buf_cap - 1U;
  in_ptr = (char *)(void *)ws;
  out_ptr = buf;

  while (in_left > 0U) {
    size_t rc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
    if (rc != (size_t)-1) {
      break;
    }
    if (errno == E2BIG) {
      break;
    }
    (void)iconv_close(cd);
    return s_encoding_error_placeholder;
  }

  (void)iconv_close(cd);
  *out_ptr = '\0';
  return buf;
#endif
}

static size_t _nei_log_serialize_event(uint8_t *out,
                                       size_t out_cap,
                                       nei_log_config_handle_t config_handle,
                                       const char *file,
                                       int32_t line,
                                       const char *func,
                                       int32_t level,
                                       int32_t verbose,
                                       const char *fmt,
                                       va_list args) {
  size_t used;
  size_t aligned_size;
  const char *scan_ptr;
  nei_log_event_header_st header;

  if (out == NULL || fmt == NULL || out_cap < sizeof(nei_log_event_header_st)) {
    return 0;
  }

  memset(&header, 0, sizeof(header));
  header.timestamp_ns = _nei_log_now_ns();
  header.config_handle = config_handle;
  header.file_ptr = file;
  header.func_ptr = func;
  header.fmt_ptr = fmt;
  header.level = level;
  header.line = line;
  header.verbose = verbose;
  _nei_log_header_fill_thread_id(&header, config_handle);

  /* Reserve header space; write header once after total_size is known. */
  used = sizeof(header);
  scan_ptr = fmt;

  while (*scan_ptr) {
    const char *p = scan_ptr;
    uint8_t payload_type = 0;
    int conv_lm = 0;
    if (*p != '%') {
      ++scan_ptr;
      continue;
    }
    ++p;
    if (*p == '%') {
      scan_ptr = p + 1;
      continue;
    }
    while (*p && _nei_log_is_flag_char(*p))
      ++p;
    if (*p == '*') {
      int32_t v = va_arg(args, int);
      if (_nei_log_payload_write_u8(out, out_cap, &used, _NEI_LOG_PAYLOAD_I32) != 0)
        return 0;
      if (_nei_log_payload_write_bytes(out, out_cap, &used, &v, sizeof(v)) != 0)
        return 0;
      ++p;
    } else {
      while (*p && _nei_log_is_digit_char(*p))
        ++p;
    }
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        int32_t v = va_arg(args, int);
        if (_nei_log_payload_write_u8(out, out_cap, &used, _NEI_LOG_PAYLOAD_I32) != 0)
          return 0;
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &v, sizeof(v)) != 0)
          return 0;
        ++p;
      } else {
        while (*p && _nei_log_is_digit_char(*p))
          ++p;
      }
    }
    if (*p == 'h') {
      ++p;
      if (*p == 'h') {
        ++p;
        conv_lm = 1;
      } else {
        conv_lm = 2;
      }
    } else if (*p == 'l') {
      ++p;
      if (*p == 'l') {
        ++p;
        conv_lm = 4;
      } else {
        conv_lm = 3;
      }
    } else if (*p == 'j') {
      ++p;
      conv_lm = 5;
    } else if (*p == 'z') {
      ++p;
      conv_lm = 6;
    } else if (*p == 't') {
      ++p;
      conv_lm = 7;
    } else if (*p == 'L') {
      ++p;
      conv_lm = 8;
    }
    if (*p == '\0') {
      break;
    }
    /* Reject %n: writing through a captured pointer is unsafe for async/binary logs. */
    if (*p == 'n') {
      return 0;
    }
    switch (*p) {
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
    case 'a':
    case 'A':
      payload_type = (conv_lm == 8) ? _NEI_LOG_PAYLOAD_LONGDOUBLE : _NEI_LOG_PAYLOAD_DOUBLE;
      break;
    case 'd':
    case 'i':
      if (conv_lm == 0 || conv_lm == 1 || conv_lm == 2) {
        payload_type = _NEI_LOG_PAYLOAD_I32;
      } else {
        payload_type = _NEI_LOG_PAYLOAD_I64;
      }
      break;
    case 'u':
    case 'x':
    case 'X':
    case 'o':
      if (conv_lm == 0 || conv_lm == 1 || conv_lm == 2) {
        payload_type = _NEI_LOG_PAYLOAD_U32;
      } else {
        payload_type = _NEI_LOG_PAYLOAD_U64;
      }
      break;
    case 'c':
      payload_type = _NEI_LOG_PAYLOAD_CHAR;
      break;
    case 's':
      payload_type = _NEI_LOG_PAYLOAD_CSTR;
      break;
    case 'p':
      payload_type = _NEI_LOG_PAYLOAD_PTR;
      break;
    default:
      payload_type = 0;
      break;
    }
    if (payload_type != 0) {
      if (_nei_log_payload_write_u8(out, out_cap, &used, payload_type) != 0)
        return 0;
      switch (payload_type) {
      case _NEI_LOG_PAYLOAD_I32: {
        int32_t v = (int32_t)va_arg(args, int);
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &v, sizeof(v)) != 0)
          return 0;
        break;
      }
      case _NEI_LOG_PAYLOAD_U32: {
        uint32_t v = (uint32_t)va_arg(args, unsigned int);
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &v, sizeof(v)) != 0)
          return 0;
        break;
      }
      case _NEI_LOG_PAYLOAD_I64: {
        int64_t v64 = 0;
        if (conv_lm == 3) {
          v64 = (int64_t)va_arg(args, long);
        } else if (conv_lm == 4) {
          v64 = (int64_t)va_arg(args, long long);
        } else if (conv_lm == 5) {
          v64 = (int64_t)va_arg(args, intmax_t);
        } else if (conv_lm == 6) {
          v64 = (int64_t)va_arg(args, ptrdiff_t);
        } else if (conv_lm == 7) {
          v64 = (int64_t)va_arg(args, ptrdiff_t);
        } else {
          v64 = (int64_t)va_arg(args, long long);
        }
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &v64, sizeof(v64)) != 0)
          return 0;
        break;
      }
      case _NEI_LOG_PAYLOAD_U64: {
        uint64_t v64 = 0;
        if (conv_lm == 3) {
          v64 = (uint64_t)va_arg(args, unsigned long);
        } else if (conv_lm == 4) {
          v64 = (uint64_t)va_arg(args, unsigned long long);
        } else if (conv_lm == 5) {
          v64 = (uint64_t)va_arg(args, uintmax_t);
        } else if (conv_lm == 6 || conv_lm == 7) {
          v64 = (uint64_t)va_arg(args, size_t);
        } else {
          v64 = (uint64_t)va_arg(args, unsigned long long);
        }
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &v64, sizeof(v64)) != 0)
          return 0;
        break;
      }
      case _NEI_LOG_PAYLOAD_DOUBLE: {
        double v = va_arg(args, double);
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &v, sizeof(v)) != 0)
          return 0;
        break;
      }
      case _NEI_LOG_PAYLOAD_LONGDOUBLE: {
        long double ld = va_arg(args, long double);
        uint8_t raw[_NEI_LOG_LONGDOUBLE_STORAGE];
        memset(raw, 0, sizeof(raw));
        memcpy(raw, &ld, sizeof(ld) < sizeof(raw) ? sizeof(ld) : sizeof(raw));
        if (_nei_log_payload_write_bytes(out, out_cap, &used, raw, sizeof(raw)) != 0)
          return 0;
        break;
      }
      case _NEI_LOG_PAYLOAD_CHAR: {
        char ch = (char)va_arg(args, int);
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &ch, sizeof(ch)) != 0)
          return 0;
        break;
      }
      case _NEI_LOG_PAYLOAD_PTR: {
        uintptr_t raw = (uintptr_t)va_arg(args, const void *);
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &raw, sizeof(raw)) != 0)
          return 0;
        break;
      }
      case _NEI_LOG_PAYLOAD_CSTR: {
        const char *s = NULL;
        char ws_buf[_NEI_LOG_MAX_STRING_COPY + 1U];
        uint16_t len16;
        size_t len;
        if (conv_lm == 3) {
          const wchar_t *ws = va_arg(args, const wchar_t *);
          s = _nei_log_wstr_to_mbs_or_placeholder(ws, ws_buf, sizeof(ws_buf));
        } else {
          s = va_arg(args, const char *);
          if (s == NULL)
            s = "(null)";
        }
        len = strlen(s);
        if (len > _NEI_LOG_MAX_STRING_COPY)
          len = _NEI_LOG_MAX_STRING_COPY;
        len16 = (uint16_t)len;
        if (_nei_log_payload_write_u16(out, out_cap, &used, len16) != 0)
          return 0;
        if (_nei_log_payload_write_bytes(out, out_cap, &used, s, len) != 0)
          return 0;
        break;
      }
      default:
        return 0;
      }
    }
    scan_ptr = (*p) ? (p + 1) : p;
  }

  aligned_size = _nei_log_align_up_8(used);
  if (_nei_log_payload_write_padded_zero(out, out_cap, &used, aligned_size) != 0) {
    return 0;
  }

  header.total_size = (uint32_t)used;
  memcpy(out, &header, sizeof(header));
  return used;
}

static size_t _nei_log_serialize_literal_msg(uint8_t *out,
                                             size_t out_cap,
                                             nei_log_config_handle_t config_handle,
                                             const char *file,
                                             int32_t line,
                                             const char *func,
                                             int32_t level,
                                             int32_t verbose,
                                             const char *message,
                                             size_t message_length) {
  nei_log_event_header_st header;
  size_t used;
  size_t aligned_size;
  size_t copy_len;
  uint16_t len16;

  if (out == NULL || out_cap < sizeof(nei_log_event_header_st)) {
    return 0;
  }

  copy_len = message_length;
  if (message == NULL) {
    copy_len = 0U;
  } else if (copy_len > _NEI_LOG_MAX_STRING_COPY) {
    copy_len = _NEI_LOG_MAX_STRING_COPY;
  }
  len16 = (uint16_t)copy_len;

  memset(&header, 0, sizeof(header));
  header.timestamp_ns = _nei_log_now_ns();
  header.config_handle = config_handle;
  header.file_ptr = file;
  header.func_ptr = func;
  header.fmt_ptr = NULL;
  header.level = level;
  header.line = line;
  header.verbose = verbose;
  _nei_log_header_fill_thread_id(&header, config_handle);

  /* Reserve header space; write header once after total_size is known. */
  used = sizeof(header);

  if (_nei_log_payload_write_u8(out, out_cap, &used, _NEI_LOG_PAYLOAD_LITERAL_MSG) != 0) {
    return 0;
  }
  if (_nei_log_payload_write_u16(out, out_cap, &used, len16) != 0) {
    return 0;
  }
  if (copy_len > 0U && message != NULL) {
    if (_nei_log_payload_write_bytes(out, out_cap, &used, message, copy_len) != 0) {
      return 0;
    }
  }

  aligned_size = _nei_log_align_up_8(used);
  if (_nei_log_payload_write_padded_zero(out, out_cap, &used, aligned_size) != 0) {
    return 0;
  }

  header.total_size = (uint32_t)used;
  memcpy(out, &header, sizeof(header));
  return used;
}

#pragma endregion /* log core (serialization) */

#pragma region log core (formatting)

typedef struct _nei_log_ts_cache_st {
  int ready;
  time_t sec;
  nei_log_timestamp_style_e style;
  char datetime[48];
  size_t datetime_len;
  char tz[16];
  int has_tz;
} nei_log_ts_cache_st;

static _NEI_LOG_TLS nei_log_ts_cache_st s_tls_ts_cache;

static int _nei_log_format_tz_offset(time_t sec, char *out, size_t out_size) {
  struct tm gm_tm;
  time_t gm_as_local;
  long offset_sec;
  long abs_sec;
  long hh;
  long mm;

  if (out == NULL || out_size < 7U) {
    return -1;
  }
#if defined(_WIN32)
  gmtime_s(&gm_tm, &sec);
#else
  gmtime_r(&sec, &gm_tm);
#endif
  gm_as_local = mktime(&gm_tm);
  if (gm_as_local == (time_t)-1) {
    return -1;
  }
  offset_sec = (long)difftime(sec, gm_as_local);
  abs_sec = labs(offset_sec);
  hh = abs_sec / 3600L;
  mm = (abs_sec % 3600L) / 60L;
  (void)snprintf(out, out_size, "%c%02ld:%02ld", (offset_sec >= 0L) ? '+' : '-', hh, mm);
  return 0;
}

static void
_nei_log_format_timestamp(uint64_t timestamp_ns, nei_log_timestamp_style_e style, char *out, size_t out_size) {
  time_t sec;
  unsigned millis;
  unsigned nanos;
  struct tm tm_buf;
  size_t n = 0U;
  int cache_hit;
  nei_log_ts_cache_st *cache = &s_tls_ts_cache;

  if (out == NULL || out_size == 0U) {
    return;
  }
  out[0] = '\0';

  if (style == NEI_LOG_TIMESTAMP_STYLE_NONE) {
    return;
  }

  sec = (time_t)(timestamp_ns / 1000000000ULL);
  millis = (unsigned)((timestamp_ns % 1000000000ULL) / 1000000ULL);
  nanos = (unsigned)(timestamp_ns % 1000000000ULL);

  cache_hit = cache->ready && cache->sec == sec && cache->style == style;
  if (!cache_hit) {
#if defined(_WIN32)
    localtime_s(&tm_buf, &sec);
#else
    localtime_r(&sec, &tm_buf);
#endif
    cache->has_tz = 0;
    cache->tz[0] = '\0';
    cache->datetime_len = 0U;

    switch (style) {
    case NEI_LOG_TIMESTAMP_STYLE_ISO8601_MS:
      n = strftime(cache->datetime, sizeof(cache->datetime), "%Y-%m-%dT%H:%M:%S", &tm_buf);
      break;
    case NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS:
    case NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS_NSEC:
      n = strftime(cache->datetime, sizeof(cache->datetime), "%Y-%m-%dT%H:%M:%S", &tm_buf);
      if (n > 0U) {
        if (_nei_log_format_tz_offset(sec, cache->tz, sizeof(cache->tz)) != 0) {
          (void)snprintf(cache->tz, sizeof(cache->tz), "+00:00");
        }
        cache->has_tz = 1;
      }
      break;
    case NEI_LOG_TIMESTAMP_STYLE_DEFAULT:
    default:
      n = strftime(cache->datetime, sizeof(cache->datetime), "%Y-%m-%d %H:%M:%S", &tm_buf);
      break;
    }
    if (n > 0U) {
      cache->datetime_len = n;
      cache->sec = sec;
      cache->style = style;
      cache->ready = 1;
    }
  }

  if (cache->ready && cache->sec == sec && cache->style == style) {
    if (cache->has_tz) {
      if (style == NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS_NSEC) {
        (void)snprintf(out, out_size, "%s.%09u%s", cache->datetime, nanos, cache->tz);
      } else {
        (void)snprintf(out, out_size, "%s.%03u%s", cache->datetime, millis, cache->tz);
      }
    } else {
      (void)snprintf(out, out_size, "%s.%03u", cache->datetime, millis);
    }
    return;
  }

  /* Conservative fallback when cache fill/formatting fails. */
#if defined(_WIN32)
  localtime_s(&tm_buf, &sec);
#else
  localtime_r(&sec, &tm_buf);
#endif
  if (style == NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS || style == NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS_NSEC) {
    char tzbuf[16];
    if (_nei_log_format_tz_offset(sec, tzbuf, sizeof(tzbuf)) != 0) {
      (void)snprintf(tzbuf, sizeof(tzbuf), "+00:00");
    }
    if (style == NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS_NSEC) {
      (void)snprintf(out,
                     out_size,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%09u%s",
                     tm_buf.tm_year + 1900,
                     tm_buf.tm_mon + 1,
                     tm_buf.tm_mday,
                     tm_buf.tm_hour,
                     tm_buf.tm_min,
                     tm_buf.tm_sec,
                     nanos,
                     tzbuf);
    } else {
      (void)snprintf(out,
                     out_size,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%03u%s",
                     tm_buf.tm_year + 1900,
                     tm_buf.tm_mon + 1,
                     tm_buf.tm_mday,
                     tm_buf.tm_hour,
                     tm_buf.tm_min,
                     tm_buf.tm_sec,
                     millis,
                     tzbuf);
    }
    return;
  }
  if (style == NEI_LOG_TIMESTAMP_STYLE_ISO8601_MS) {
    (void)snprintf(out,
                   out_size,
                   "%04d-%02d-%02dT%02d:%02d:%02d.%03u",
                   tm_buf.tm_year + 1900,
                   tm_buf.tm_mon + 1,
                   tm_buf.tm_mday,
                   tm_buf.tm_hour,
                   tm_buf.tm_min,
                   tm_buf.tm_sec,
                   millis);
  } else {
    (void)snprintf(out,
                   out_size,
                   "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                   tm_buf.tm_year + 1900,
                   tm_buf.tm_mon + 1,
                   tm_buf.tm_mday,
                   tm_buf.tm_hour,
                   tm_buf.tm_min,
                   tm_buf.tm_sec,
                   millis);
  }
}

static int _nei_log_append_char(char *out, size_t cap, size_t *used, char c) {
  if (out == NULL || used == NULL || *used + 1U >= cap) {
    return -1;
  }
  out[*used] = c;
  *used += 1U;
  out[*used] = '\0';
  return 0;
}

static int _nei_log_append_nstr(char *out, size_t cap, size_t *used, const char *s, size_t n) {
  if (out == NULL || used == NULL || s == NULL || *used + n >= cap) {
    return -1;
  }
  memcpy(out + *used, s, n);
  *used += n;
  out[*used] = '\0';
  return 0;
}

static int _nei_log_append_cstr(char *out, size_t cap, size_t *used, const char *s) {
  return _nei_log_append_nstr(out, cap, used, s, strlen(s));
}

static int _nei_log_read_u16(const uint8_t **cursor, const uint8_t *end, uint16_t *out_value) {
  if (cursor == NULL || *cursor == NULL || end == NULL || out_value == NULL || *cursor + sizeof(uint16_t) > end) {
    return -1;
  }
  memcpy(out_value, *cursor, sizeof(uint16_t));
  *cursor += sizeof(uint16_t);
  return 0;
}

static int _nei_log_read_bytes(const uint8_t **cursor, const uint8_t *end, void *dst, size_t n) {
  if (cursor == NULL || *cursor == NULL || end == NULL || dst == NULL || *cursor + n > end) {
    return -1;
  }
  memcpy(dst, *cursor, n);
  *cursor += n;
  return 0;
}

static void _nei_log_snprintf_i64(const char *spec, char *tmp, size_t tcap, int64_t v) {
  if (strstr(spec, "ll") != NULL) {
    snprintf(tmp, tcap, spec, (long long)v);
  } else if (strchr(spec, 'j')) {
    snprintf(tmp, tcap, spec, (intmax_t)v);
  } else if (strchr(spec, 'z') && strchr(spec, 'u')) {
    snprintf(tmp, tcap, spec, (size_t)(uint64_t)v);
  } else if (strchr(spec, 'z')) {
    snprintf(tmp, tcap, spec, (ptrdiff_t)v);
  } else if (strchr(spec, 't')) {
    if (strchr(spec, 'u') || strchr(spec, 'x') || strchr(spec, 'X') || strchr(spec, 'o')) {
      snprintf(tmp, tcap, spec, (size_t)(uint64_t)v);
    } else {
      snprintf(tmp, tcap, spec, (ptrdiff_t)v);
    }
  } else if (strchr(spec, 'l')) {
    snprintf(tmp, tcap, spec, (long)v);
  } else {
    snprintf(tmp, tcap, spec, (int)v);
  }
}

static void _nei_log_snprintf_u64(const char *spec, char *tmp, size_t tcap, uint64_t v) {
  if (strstr(spec, "ll") != NULL) {
    snprintf(tmp, tcap, spec, (unsigned long long)v);
  } else if (strchr(spec, 'j')) {
    snprintf(tmp, tcap, spec, (uintmax_t)v);
  } else if (strchr(spec, 'z')) {
    snprintf(tmp, tcap, spec, (size_t)v);
  } else if (strchr(spec, 't')) {
    snprintf(tmp, tcap, spec, (size_t)v);
  } else if (strchr(spec, 'l')) {
    snprintf(tmp, tcap, spec, (unsigned long)v);
  } else {
    snprintf(tmp, tcap, spec, (unsigned int)v);
  }
}

static int _nei_log_build_runtime_conversion_spec(const char *scan,
                                                  const char **after,
                                                  const uint8_t **cursor,
                                                  const uint8_t *end,
                                                  uint8_t *parsed_args,
                                                  char *spec,
                                                  size_t spec_cap) {
  const char *p;
  size_t n = 0U;
  if (scan == NULL || after == NULL || cursor == NULL || *cursor == NULL || end == NULL || parsed_args == NULL
      || spec == NULL || spec_cap < 3U || *scan != '%') {
    return -1;
  }

  p = scan;
  spec[n++] = *p++;
  if (*p == '%') {
    if (n + 2U >= spec_cap) {
      return -1;
    }
    spec[n++] = *p++;
    spec[n] = '\0';
    *after = p;
    return 0;
  }

  while (*p && _nei_log_is_flag_char(*p)) {
    if (n + 2U >= spec_cap)
      return -1;
    spec[n++] = *p++;
  }
  if (*p == '*') {
    int32_t width = 0;
    char tmp[16];
    uint8_t payload_type;
    if (*cursor >= end || *parsed_args == UINT8_MAX)
      return -1;
    payload_type = **cursor;
    (*cursor)++;
    if (payload_type != _NEI_LOG_PAYLOAD_I32)
      return -1;
    if (_nei_log_read_bytes(cursor, end, &width, sizeof(width)) != 0)
      return -1;
    (*parsed_args)++;
    snprintf(tmp, sizeof(tmp), "%d", (int)width);
    if (n + strlen(tmp) + 2U >= spec_cap)
      return -1;
    memcpy(spec + n, tmp, strlen(tmp));
    n += strlen(tmp);
    ++p;
  } else {
    while (*p && _nei_log_is_digit_char(*p)) {
      if (n + 2U >= spec_cap)
        return -1;
      spec[n++] = *p++;
    }
  }
  if (*p == '.') {
    if (n + 2U >= spec_cap)
      return -1;
    spec[n++] = *p++;
    if (*p == '*') {
      int32_t precision = 0;
      char tmp[16];
      uint8_t payload_type;
      if (*cursor >= end || *parsed_args == UINT8_MAX)
        return -1;
      payload_type = **cursor;
      (*cursor)++;
      if (payload_type != _NEI_LOG_PAYLOAD_I32)
        return -1;
      if (_nei_log_read_bytes(cursor, end, &precision, sizeof(precision)) != 0)
        return -1;
      (*parsed_args)++;
      if (precision < 0) {
        precision = 0;
      }
      snprintf(tmp, sizeof(tmp), "%d", (int)precision);
      if (n + strlen(tmp) + 2U >= spec_cap)
        return -1;
      memcpy(spec + n, tmp, strlen(tmp));
      n += strlen(tmp);
      ++p;
    } else {
      while (*p && _nei_log_is_digit_char(*p)) {
        if (n + 2U >= spec_cap)
          return -1;
        spec[n++] = *p++;
      }
    }
  }
  if (*p == 'h' || *p == 'l') {
    char first = *p;
    ++p;
    if (*p == first) {
      if (n + 3U >= spec_cap)
        return -1;
      spec[n++] = first;
      spec[n++] = *p++;
    } else if (first == 'l' && *p == 's') {
      /* Runtime formatting always replays narrow bytes, so keep %ls as %s in replay spec. */
    } else {
      if (n + 2U >= spec_cap)
        return -1;
      spec[n++] = first;
    }
  } else if (*p == 'j' || *p == 'z' || *p == 't') {
    if (n + 2U >= spec_cap)
      return -1;
    spec[n++] = *p++;
  } else if (*p == 'L') {
    if (n + 2U >= spec_cap)
      return -1;
    spec[n++] = *p++;
  }
  if (*p == '\0') {
    return -1;
  }
  if (n + 2U >= spec_cap) {
    return -1;
  }
  spec[n++] = *p++;
  spec[n] = '\0';
  *after = p;
  return 0;
}

static const char *_nei_log_basename(const char *path) {
  const char *slash;
  const char *backslash;
  if (path == NULL) {
    return NULL;
  }
  slash = strrchr(path, '/');
  backslash = strrchr(path, '\\');
  if (slash == NULL && backslash == NULL) {
    return path;
  }
  if (slash == NULL) {
    return backslash + 1;
  }
  if (backslash == NULL) {
    return slash + 1;
  }
  return (slash > backslash) ? (slash + 1) : (backslash + 1);
}

static int _nei_log_append_location_block(
    const nei_log_event_header_st *header, int short_path, char *out, size_t out_cap, size_t *used) {
  if (header == NULL || out == NULL || used == NULL) {
    return -1;
  }

  if (header->file_ptr != NULL) {
    const char *path = short_path ? _nei_log_basename(header->file_ptr) : header->file_ptr;
    if (_nei_log_append_cstr(out, out_cap, used, path) != 0)
      return -1;
    if (_nei_log_append_char(out, out_cap, used, ':') != 0)
      return -1;
    {
      char line_buf[16];
      snprintf(line_buf, sizeof(line_buf), "%d", (int)header->line);
      if (_nei_log_append_cstr(out, out_cap, used, line_buf) != 0)
        return -1;
    }
  }

  if (header->func_ptr != NULL) {
    if (header->file_ptr != NULL) {
      if (_nei_log_append_char(out, out_cap, used, ' ') != 0)
        return -1;
    }
    if (_nei_log_append_cstr(out, out_cap, used, header->func_ptr) != 0)
      return -1;
  }

  return 0;
}

static int _nei_log_format_event(const nei_log_event_header_st *header,
                                 const nei_log_config_st *effective_config,
                                 const uint8_t *payload,
                                 size_t payload_size,
                                 char *out,
                                 size_t out_cap) {
  const char *fmt;
  const char *scan;
  const uint8_t *cursor = payload;
  const uint8_t *end = payload + payload_size;
  size_t used = 0U;
  uint8_t parsed_args = 0U;
  int short_level_tag;
  int short_path;
  int log_location;
  int log_location_after_message;
  nei_log_timestamp_style_e ts_style;

  if (header == NULL || effective_config == NULL || payload == NULL || out == NULL || out_cap == 0U) {
    return -1;
  }
  short_level_tag = effective_config->short_level_tag;
  short_path = effective_config->short_path;
  log_location = effective_config->log_location;
  log_location_after_message = effective_config->log_location_after_message;
  ts_style = effective_config->timestamp_style;
  out[0] = '\0';
  {
    char ts[80];
    _nei_log_format_timestamp(header->timestamp_ns, ts_style, ts, sizeof(ts));
    if (ts[0] != '\0') {
      if (_nei_log_append_char(out, out_cap, &used, '[') != 0)
        return -1;
      if (_nei_log_append_cstr(out, out_cap, &used, ts) != 0)
        return -1;
      if (_nei_log_append_cstr(out, out_cap, &used, "] ") != 0)
        return -1;
    }
  }

  if (header->verbose == _NEI_LOG_NOT_VERBOSE) {
    if (_nei_log_append_char(out, out_cap, &used, '[') != 0)
      return -1;
    if (_nei_log_append_cstr(out,
                             out_cap,
                             &used,
                             short_level_tag ? _get_level_short_string((nei_log_level_e)header->level)
                                             : _get_level_string((nei_log_level_e)header->level))
        != 0)
      return -1;
    if (_nei_log_append_cstr(out, out_cap, &used, "] ") != 0)
      return -1;
  } else {
    if (_nei_log_append_cstr(out, out_cap, &used, "[V] ") != 0)
      return -1;
  }
  if (header->thread_id_len > 0U) {
    if (_nei_log_append_cstr(out, out_cap, &used, "tid=") != 0)
      return -1;
    if (_nei_log_append_nstr(out, out_cap, &used, header->thread_id_str, (size_t)header->thread_id_len) != 0)
      return -1;
    if (_nei_log_append_cstr(out, out_cap, &used, " ") != 0)
      return -1;
  }
  if (log_location != 0 && log_location_after_message == 0) {
    if (_nei_log_append_location_block(header, short_path, out, out_cap, &used) != 0)
      return -1;
    if ((header->file_ptr != NULL || header->func_ptr != NULL) && _nei_log_append_cstr(out, out_cap, &used, " - ") != 0)
      return -1;
  }

  if (header->fmt_ptr == NULL) {
    if (cursor >= end) {
      return -1;
    }
    if (*cursor != _NEI_LOG_PAYLOAD_LITERAL_MSG) {
      return -1;
    }
    ++cursor;
    {
      uint16_t len16 = 0;
      if (_nei_log_read_u16(&cursor, end, &len16) != 0) {
        return -1;
      }
      if (len16 > _NEI_LOG_MAX_STRING_COPY || cursor + len16 > end) {
        return -1;
      }
      if (len16 > 0U && _nei_log_append_nstr(out, out_cap, &used, (const char *)cursor, len16) != 0) {
        return -1;
      }
    }
    if (log_location != 0 && log_location_after_message != 0
        && (header->file_ptr != NULL || header->func_ptr != NULL)) {
      if (_nei_log_append_cstr(out, out_cap, &used, " - ") != 0)
        return -1;
      if (_nei_log_append_location_block(header, short_path, out, out_cap, &used) != 0)
        return -1;
    }
    return 0;
  }

  fmt = header->fmt_ptr;
  scan = fmt;
  while (*scan) {
    if (*scan != '%') {
      if (_nei_log_append_char(out, out_cap, &used, *scan) != 0)
        return -1;
      ++scan;
      continue;
    }
    {
      char conv_spec[64];
      const char *after = NULL;
      if (_nei_log_build_runtime_conversion_spec(scan, &after, &cursor, end, &parsed_args, conv_spec, sizeof(conv_spec))
          != 0) {
        return -1;
      }
      if (strcmp(conv_spec, "%%") == 0) {
        if (_nei_log_append_char(out, out_cap, &used, '%') != 0)
          return -1;
        scan = after;
        continue;
      }
      if (cursor >= end) {
        return -1;
      }
      {
        const uint8_t payload_type = *cursor++;
        char tmp[256];
        switch (payload_type) {
        case _NEI_LOG_PAYLOAD_I32: {
          int32_t v = 0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          snprintf(tmp, sizeof(tmp), conv_spec, (int)v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_U32: {
          uint32_t v = 0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          snprintf(tmp, sizeof(tmp), conv_spec, (unsigned int)v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_I64: {
          int64_t v = 0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          _nei_log_snprintf_i64(conv_spec, tmp, sizeof(tmp), v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_U64: {
          uint64_t v = 0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          _nei_log_snprintf_u64(conv_spec, tmp, sizeof(tmp), v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_DOUBLE: {
          double v = 0.0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          snprintf(tmp, sizeof(tmp), conv_spec, v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_LONGDOUBLE: {
          uint8_t raw[_NEI_LOG_LONGDOUBLE_STORAGE];
          long double ld = 0.0L;
          if (_nei_log_read_bytes(&cursor, end, raw, sizeof(raw)) != 0)
            return -1;
          memset(&ld, 0, sizeof(ld));
          memcpy(&ld, raw, sizeof(ld) < sizeof(raw) ? sizeof(ld) : sizeof(raw));
          snprintf(tmp, sizeof(tmp), conv_spec, ld);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_CHAR: {
          char v = '\0';
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          snprintf(tmp, sizeof(tmp), conv_spec, (int)v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_PTR: {
          uintptr_t raw = 0U;
          void *ptr = NULL;
          if (_nei_log_read_bytes(&cursor, end, &raw, sizeof(raw)) != 0)
            return -1;
          ptr = (void *)raw;
          snprintf(tmp, sizeof(tmp), conv_spec, ptr);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_CSTR: {
          uint16_t len16 = 0;
          char strbuf[_NEI_LOG_MAX_STRING_COPY + 1U];
          if (_nei_log_read_u16(&cursor, end, &len16) != 0)
            return -1;
          if (len16 > _NEI_LOG_MAX_STRING_COPY || cursor + len16 > end)
            return -1;
          memcpy(strbuf, cursor, len16);
          strbuf[len16] = '\0';
          cursor += len16;
          snprintf(tmp, sizeof(tmp), conv_spec, strbuf);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        default:
          return -1;
        }
        ++parsed_args;
      }
      scan = after;
      continue;
    }
  }

  if (log_location != 0 && log_location_after_message != 0 && (header->file_ptr != NULL || header->func_ptr != NULL)) {
    if (_nei_log_append_cstr(out, out_cap, &used, " - ") != 0)
      return -1;
    if (_nei_log_append_location_block(header, short_path, out, out_cap, &used) != 0)
      return -1;
  }
  return 0;
}

#pragma endregion /* log core (formatting) */

#pragma region log core (emit)

static void _nei_log_emit_message(
    const nei_log_config_st *config, int32_t level, int32_t verbose, const char *message, size_t length) {
  const nei_log_config_st *effective = config;
  size_t i;
  if (config == NULL || message == NULL) {
    return;
  }
  if (verbose == _NEI_LOG_NOT_VERBOSE) {
    const uint32_t mask = (uint32_t)(1U << (uint32_t)level);
    if (level < (int32_t)NEI_L_VERBOSE || level > (int32_t)NEI_L_FATAL) {
      return;
    }
    if ((effective->level_flags.all & mask) == 0U) {
      return;
    }
  } else if (effective->verbose_threshold >= 0 && verbose > effective->verbose_threshold) {
    return;
  }
  for (i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
    nei_log_sink_st *sink = effective->sinks[i];
    if (sink == NULL) {
      break;
    }
    if (verbose != _NEI_LOG_NOT_VERBOSE) {
      if (sink->vlog != NULL) {
        sink->vlog(sink, verbose, message, length);
      }
    } else {
      if (sink->llog != NULL) {
        sink->llog(sink, (nei_log_level_e)level, message, length);
      }
    }
  }
  if (effective->log_to_console) {
    (void)fwrite(message, 1U, length, stdout);
    (void)fputc('\n', stdout);
  }
}

#pragma endregion /* log core (emit) */

#pragma region log core (runtime)

static int _nei_log_ensure_runtime_initialized(void) {
  if (s_runtime.initialized) {
    return 0;
  }

#if defined(_WIN32)
  InitializeCriticalSection(&s_runtime.mutex);
  InitializeConditionVariable(&s_runtime.cond);
  s_runtime.thread = CreateThread(NULL, 0, _nei_log_consumer_thread, &s_runtime, 0, NULL);
  if (s_runtime.thread == NULL) {
    DeleteCriticalSection(&s_runtime.mutex);
    return -1;
  }
#else
  if (pthread_mutex_init(&s_runtime.mutex, NULL) != 0) {
    return -1;
  }
  if (pthread_cond_init(&s_runtime.cond, NULL) != 0) {
    pthread_mutex_destroy(&s_runtime.mutex);
    return -1;
  }
  if (pthread_create(&s_runtime.thread, NULL, _nei_log_consumer_thread, &s_runtime) != 0) {
    pthread_cond_destroy(&s_runtime.cond);
    pthread_mutex_destroy(&s_runtime.mutex);
    return -1;
  }
#endif

  s_runtime.initialized = 1;
  atexit(_nei_log_shutdown_runtime);
  return 0;
}

static void _nei_log_shutdown_runtime(void) {
  if (!s_runtime.initialized) {
    return;
  }

#if defined(_WIN32)
  EnterCriticalSection(&s_runtime.mutex);
  s_runtime.stop_requested = 1;
  WakeAllConditionVariable(&s_runtime.cond);
  LeaveCriticalSection(&s_runtime.mutex);
  WaitForSingleObject(s_runtime.thread, INFINITE);
  CloseHandle(s_runtime.thread);
  s_runtime.consumer_thread_id = 0U;
  DeleteCriticalSection(&s_runtime.mutex);
#else
  pthread_mutex_lock(&s_runtime.mutex);
  s_runtime.stop_requested = 1;
  pthread_cond_broadcast(&s_runtime.cond);
  pthread_mutex_unlock(&s_runtime.mutex);
  pthread_join(s_runtime.thread, NULL);
  pthread_cond_destroy(&s_runtime.cond);
  pthread_mutex_destroy(&s_runtime.mutex);
#endif
  s_runtime.initialized = 0;
}

static void _nei_log_ensure_active_not_consuming(nei_log_runtime_st *rt) {
  while (rt->consuming_index >= 0 && rt->active_index == rt->consuming_index) {
#if defined(_WIN32)
    SleepConditionVariableCS(&rt->cond, &rt->mutex, INFINITE);
#else
    pthread_cond_wait(&rt->cond, &rt->mutex);
#endif
  }
}

static int _nei_log_enqueue_event(const uint8_t *event, size_t len) {
  if (event == NULL || len == 0U || len > _NEI_LOG_GLOBAL_BUFFER_CAPACITY || !s_runtime.initialized) {
    return -1;
  }

#if defined(_WIN32)
  EnterCriticalSection(&s_runtime.mutex);
#else
  pthread_mutex_lock(&s_runtime.mutex);
#endif

  for (;;) {
    const int active = s_runtime.active_index;
    const size_t free_space = _NEI_LOG_GLOBAL_BUFFER_CAPACITY - s_runtime.used[active];
    if (free_space >= len) {
      uint8_t *dst = (active == 0) ? s_runtime.buffer_a : s_runtime.buffer_b;
      memcpy(dst + s_runtime.used[active], event, len);
      s_runtime.used[active] += len;
      if (s_runtime.pending_index == -1) {
        s_runtime.pending_index = active;
        s_runtime.active_index = 1 - active;
        _nei_log_ensure_active_not_consuming(&s_runtime);
#if defined(_WIN32)
        WakeAllConditionVariable(&s_runtime.cond);
#else
        pthread_cond_broadcast(&s_runtime.cond);
#endif
      }
      break;
    }

    if (s_runtime.pending_index == -1 && s_runtime.used[active] > 0U) {
      s_runtime.pending_index = active;
      s_runtime.active_index = 1 - active;
      _nei_log_ensure_active_not_consuming(&s_runtime);
#if defined(_WIN32)
      WakeAllConditionVariable(&s_runtime.cond);
#else
      pthread_cond_broadcast(&s_runtime.cond);
#endif
      continue;
    }

#if defined(_WIN32)
    SleepConditionVariableCS(&s_runtime.cond, &s_runtime.mutex, INFINITE);
#else
    pthread_cond_wait(&s_runtime.cond, &s_runtime.mutex);
#endif
  }

#if defined(_WIN32)
  LeaveCriticalSection(&s_runtime.mutex);
#else
  pthread_mutex_unlock(&s_runtime.mutex);
#endif
  return 0;
}

#pragma endregion /* log core (runtime) */

#pragma region log core (consumer)

static void _nei_log_process_events(const uint8_t *buf, size_t size) {
  size_t offset = 0U;
  char message[2048];
  while (offset + sizeof(nei_log_event_header_st) <= size) {
    const nei_log_config_st *config = NULL;
    const nei_log_event_header_st *header = (const nei_log_event_header_st *)(buf + offset);
    const size_t payload_size = (size_t)header->total_size - sizeof(nei_log_event_header_st);
    const uint8_t *payload = buf + offset + sizeof(nei_log_event_header_st);
    if (header->total_size == 0U || offset + header->total_size > size) {
      break;
    }
    config = nei_log_get_config(header->config_handle);
    if (config != NULL && _nei_log_format_event(header, config, payload, payload_size, message, sizeof(message)) == 0) {
      _nei_log_emit_message(config, header->level, header->verbose, message, strlen(message));
    }
    offset += header->total_size;
  }
}

#if defined(_WIN32)
static DWORD WINAPI _nei_log_consumer_thread(LPVOID arg) {
#else
static void *_nei_log_consumer_thread(void *arg) {
#endif
  nei_log_runtime_st *rt = (nei_log_runtime_st *)arg;
  if (rt == NULL) {
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
  }

#if defined(_WIN32)
  rt->consumer_thread_id = GetCurrentThreadId();
#endif

  for (;;) {
    int consume_index = -1;
    size_t consume_size = 0U;
    uint8_t *consume_buf = NULL;

#if defined(_WIN32)
    EnterCriticalSection(&rt->mutex);
    while (rt->pending_index == -1 && !rt->stop_requested) {
      SleepConditionVariableCS(&rt->cond, &rt->mutex, INFINITE);
    }
    if (rt->pending_index == -1 && rt->stop_requested) {
      if (rt->used[rt->active_index] > 0U) {
        rt->pending_index = rt->active_index;
        rt->active_index = 1 - rt->active_index;
        _nei_log_ensure_active_not_consuming(rt);
      } else {
        LeaveCriticalSection(&rt->mutex);
        break;
      }
    }
#else
    pthread_mutex_lock(&rt->mutex);
    while (rt->pending_index == -1 && !rt->stop_requested) {
      pthread_cond_wait(&rt->cond, &rt->mutex);
    }
    if (rt->pending_index == -1 && rt->stop_requested) {
      if (rt->used[rt->active_index] > 0U) {
        rt->pending_index = rt->active_index;
        rt->active_index = 1 - rt->active_index;
        _nei_log_ensure_active_not_consuming(rt);
      } else {
        pthread_mutex_unlock(&rt->mutex);
        break;
      }
    }
#endif

    consume_index = rt->pending_index;
    rt->pending_index = -1;
    rt->consuming_index = consume_index;
    consume_size = rt->used[consume_index];
    consume_buf = (consume_index == 0) ? rt->buffer_a : rt->buffer_b;

#if defined(_WIN32)
    LeaveCriticalSection(&rt->mutex);
#else
    pthread_mutex_unlock(&rt->mutex);
#endif

    if (consume_size > 0U) {
      _nei_log_process_events(consume_buf, consume_size);
    }

#if defined(_WIN32)
    EnterCriticalSection(&rt->mutex);
    rt->used[consume_index] = 0U;
    rt->consuming_index = -1;
    WakeAllConditionVariable(&rt->cond);
    LeaveCriticalSection(&rt->mutex);
#else
    pthread_mutex_lock(&rt->mutex);
    rt->used[consume_index] = 0U;
    rt->consuming_index = -1;
    pthread_cond_broadcast(&rt->cond);
    pthread_mutex_unlock(&rt->mutex);
#endif
  }

#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

#pragma endregion /* log core (consumer) */
