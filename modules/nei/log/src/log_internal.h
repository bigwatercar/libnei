#ifndef _NEI_LOG_INTERNAL_H_
#define _NEI_LOG_INTERNAL_H_

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

typedef struct _nei_log_default_file_sink_ctx_st {
  uint32_t magic;
  FILE *fp;
} nei_log_default_file_sink_ctx_st;

// NOTE: This config table is intended to be managed during initialization and
// is not designed for concurrent add/remove while logging is active.
#define _NEI_LOG_MAX_CONFIGS 16U // includes default config

/* Global variables - defined in log_config.c */
extern nei_log_config_st *s_config_ptrs[_NEI_LOG_MAX_CONFIGS];
extern nei_log_config_st s_custom_configs[_NEI_LOG_MAX_CONFIGS];
extern uint8_t s_config_used[_NEI_LOG_MAX_CONFIGS];
extern int s_config_table_initialized;
#if defined(_WIN32)
extern SRWLOCK s_config_lock;
#else
extern pthread_rwlock_t s_config_lock;
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

/* From log_runtime.c */
int _nei_log_ensure_runtime_initialized(void);
void _nei_log_shutdown_runtime(void);
int _nei_log_enqueue_event(const uint8_t *event, size_t len);
void _nei_log_ensure_active_not_consuming(nei_log_runtime_st *rt);

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
void _nei_log_emit_message(const nei_log_config_st *config, int32_t level, int32_t verbose, const char *message, size_t length);

/* From log_serialize.c */
uint64_t _nei_log_now_ns(void);

#endif /* _NEI_LOG_INTERNAL_H_ */
