#include "log_internal.h"

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
