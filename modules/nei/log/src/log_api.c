#include "log_internal.h"

/** @brief Per-thread scratch buffer for event serialization — avoids 8 KB stack allocation. */
static _NEI_LOG_TLS uint8_t _s_tls_event_buf[_NEI_LOG_EVENT_BUFFER_SIZE];
/**
 * @brief Per-thread reentrancy depth counter.
 * Guards against same-thread reentrant log calls (e.g. from a signal handler
 * or a sink that itself calls a log API). Depth > 1 → drop silently.
 */
static _NEI_LOG_TLS int _s_tls_log_depth = 0;

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

#pragma region public API

void nei_llog(nei_log_config_handle_t config_handle,
              nei_log_level_e level,
              const char *file,
              int32_t line,
              const char *func,
              const char *fmt,
              ...) {
  va_list args;
  nei_log_config_st *config;

  if (++_s_tls_log_depth > 1) {
    --_s_tls_log_depth;
    return;
  }

  (void)_nei_log_ensure_runtime_initialized();

  /* Early filter: check if this level is enabled before serialization. */
  config = nei_log_get_config(config_handle);
  if (config != NULL) {
    const uint32_t mask = (uint32_t)(1U << (uint32_t)level);
    if (level < (int32_t)NEI_L_VERBOSE || level > (int32_t)NEI_L_FATAL ||
        (config->level_flags.all & mask) == 0U) {
      --_s_tls_log_depth;
      return; /* Level not enabled, skip serialization. */
    }
  }

  va_start(args, fmt);
  {
    const size_t serialized_len = _nei_log_serialize_event(
        _s_tls_event_buf, sizeof(_s_tls_event_buf), config_handle, file, line, func, (int32_t)level, _NEI_LOG_NOT_VERBOSE, fmt, args);
    if (serialized_len > 0U) {
      (void)_nei_log_enqueue_event(_s_tls_event_buf, serialized_len);
    }
  }
  va_end(args);
  --_s_tls_log_depth;
}

void nei_vlog(nei_log_config_handle_t config_handle,
              int verbose,
              const char *file,
              int32_t line,
              const char *func,
              const char *fmt,
              ...) {
  va_list args;
  nei_log_config_st *config;

  if (++_s_tls_log_depth > 1) {
    --_s_tls_log_depth;
    return;
  }

  (void)_nei_log_ensure_runtime_initialized();

  /* Early filter: check if this verbose level passes threshold before serialization. */
  config = nei_log_get_config(config_handle);
  if (config != NULL && config->verbose_threshold >= 0 && verbose > config->verbose_threshold) {
    --_s_tls_log_depth;
    return; /* Verbose level exceeds threshold, skip serialization. */
  }

  va_start(args, fmt);
  {
    const size_t serialized_len = _nei_log_serialize_event(_s_tls_event_buf,
                                                           sizeof(_s_tls_event_buf),
                                                           config_handle,
                                                           file,
                                                           line,
                                                           func,
                                                           (int32_t)NEI_L_VERBOSE,
                                                           (int32_t)verbose,
                                                           fmt,
                                                           args);
    if (serialized_len > 0U) {
      (void)_nei_log_enqueue_event(_s_tls_event_buf, serialized_len);
    }
  }
  va_end(args);
  --_s_tls_log_depth;
}

void nei_llog_literal(nei_log_config_handle_t config_handle,
                      nei_log_level_e level,
                      const char *file,
                      int32_t line,
                      const char *func,
                      const char *message,
                      size_t length) {
  if (++_s_tls_log_depth > 1) {
    --_s_tls_log_depth;
    return;
  }

  (void)_nei_log_ensure_runtime_initialized();
  {
    const size_t serialized_len = _nei_log_serialize_literal_msg(
        _s_tls_event_buf, sizeof(_s_tls_event_buf), config_handle, file, line, func, (int32_t)level, _NEI_LOG_NOT_VERBOSE, message, length);
    if (serialized_len > 0U) {
      (void)_nei_log_enqueue_event(_s_tls_event_buf, serialized_len);
    }
  }
  --_s_tls_log_depth;
}

void nei_vlog_literal(nei_log_config_handle_t config_handle,
                      int verbose,
                      const char *file,
                      int32_t line,
                      const char *func,
                      const char *message,
                      size_t length) {
  if (++_s_tls_log_depth > 1) {
    --_s_tls_log_depth;
    return;
  }

  (void)_nei_log_ensure_runtime_initialized();
  {
    const size_t serialized_len = _nei_log_serialize_literal_msg(_s_tls_event_buf,
                                                                 sizeof(_s_tls_event_buf),
                                                                 config_handle,
                                                                 file,
                                                                 line,
                                                                 func,
                                                                 (int32_t)NEI_L_VERBOSE,
                                                                 (int32_t)verbose,
                                                                 message,
                                                                 length);
    if (serialized_len > 0U) {
      (void)_nei_log_enqueue_event(_s_tls_event_buf, serialized_len);
    }
  }
  --_s_tls_log_depth;
}

void nei_log_flush(void) {
  uint64_t flush_target;
  uint64_t consumer_pos;
  uint64_t backlog;
  uint32_t spin;
  uint32_t spin_limit;

  if (!s_runtime.initialized) {
    return;
  }
  if (_nei_log_is_consumer_thread()) {
    return;
  }

  /* Sample the current write position.  Everything enqueued before this point
   * must be consumed before flush returns.  Producers that are mid-commit will
   * finish writing their slot; the consumer will drain them as they commit. */
  flush_target = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.ring.write_pos);
  if (flush_target == 0U) {
    return;
  }
  consumer_pos = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.ring.consumer_pos);
  if (consumer_pos >= flush_target) {
    return;
  }

  backlog = flush_target - consumer_pos;
  if (backlog <= 2U) {
    spin_limit = _NEI_LOG_FLUSH_SPIN_SHALLOW_ITERS;
  } else if (backlog <= 8U) {
    spin_limit = _NEI_LOG_FLUSH_SPIN_MID_ITERS;
  } else {
    spin_limit = _NEI_LOG_FLUSH_SPIN_BASE_ITERS;
  }

  /* Adaptive spin fast path: aggressive for tiny backlog (typical per-call flush). */
  for (spin = 0U; spin < spin_limit; ++spin) {
    if (_NEI_LOG_ATOMIC_LOAD64(&s_runtime.ring.consumer_pos) >= flush_target) {
      return;
    }
    if ((spin & 31U) == 31U) {
      _NEI_LOG_THREAD_YIELD();
    } else {
      _NEI_LOG_CPU_YIELD();
    }
  }

#if defined(_WIN32)
  EnterCriticalSection(&s_runtime.mutex);
  _NEI_LOG_SIGNAL_COND(&s_runtime.cond);
  while (_NEI_LOG_ATOMIC_LOAD64(&s_runtime.ring.consumer_pos) < flush_target) {
    (void)_NEI_LOG_ATOMIC_FETCH_ADD64(&s_runtime.stat_flush_wait_loops, 1U);
    SleepConditionVariableCS(&s_runtime.cond, &s_runtime.mutex, INFINITE);
  }
  LeaveCriticalSection(&s_runtime.mutex);
#else
  pthread_mutex_lock(&s_runtime.mutex);
  _NEI_LOG_SIGNAL_COND(&s_runtime.cond);
  while (_NEI_LOG_ATOMIC_LOAD64(&s_runtime.ring.consumer_pos) < flush_target) {
    (void)_NEI_LOG_ATOMIC_FETCH_ADD64(&s_runtime.stat_flush_wait_loops, 1U);
    pthread_cond_wait(&s_runtime.cond, &s_runtime.mutex);
  }
  pthread_mutex_unlock(&s_runtime.mutex);
#endif
}

uint32_t nei_log_get_runtime_init_count_for_test(void) {
  return _nei_log_get_runtime_init_count_for_test();
}

int nei_log_get_perf_stats_for_test(nei_log_perf_stats_st *out_stats) {
  return _nei_log_get_perf_stats_for_test(out_stats);
}

void nei_log_reset_perf_stats_for_test(void) {
  _nei_log_reset_perf_stats_for_test();
}

#pragma endregion
