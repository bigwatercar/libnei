#include "log_internal.h"

#pragma region runtime

/* Forward declaration */
#if defined(_WIN32)
static DWORD WINAPI _nei_log_consumer_thread(LPVOID arg);
#else
static void *_nei_log_consumer_thread(void *arg);
#endif

nei_log_runtime_st s_runtime = {
  .used = {0U, 0U},
    .active_index = 0,
    .pending_index = -1,
    .consuming_index = -1,
    .stop_requested = 0,
    .initialized = 0,
};

int _nei_log_ensure_runtime_initialized(void) {
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

void _nei_log_shutdown_runtime(void) {
  if (!s_runtime.initialized) {
    return;
  }

  #if defined(_WIN32)
    EnterCriticalSection(&s_runtime.mutex);
    s_runtime.stop_requested = 1;
    _NEI_LOG_BROADCAST_COND(&s_runtime.cond);
    LeaveCriticalSection(&s_runtime.mutex);
    WaitForSingleObject(s_runtime.thread, INFINITE);
    CloseHandle(s_runtime.thread);
    s_runtime.consumer_thread_id = 0U;
    DeleteCriticalSection(&s_runtime.mutex);
  #else
    pthread_mutex_lock(&s_runtime.mutex);
    s_runtime.stop_requested = 1;
    _NEI_LOG_BROADCAST_COND(&s_runtime.cond);
    pthread_mutex_unlock(&s_runtime.mutex);
    pthread_join(s_runtime.thread, NULL);
    pthread_cond_destroy(&s_runtime.cond);
    pthread_mutex_destroy(&s_runtime.mutex);
  #endif
  s_runtime.initialized = 0;
}

void _nei_log_ensure_active_not_consuming(nei_log_runtime_st *rt) {
  while (rt->consuming_index >= 0 && rt->active_index == rt->consuming_index) {
#if defined(_WIN32)
    SleepConditionVariableCS(&rt->cond, &rt->mutex, INFINITE);
#else
    pthread_cond_wait(&rt->cond, &rt->mutex);
#endif
  }
}

int _nei_log_enqueue_event(const uint8_t *event, size_t len) {
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
  _NEI_LOG_SIGNAL_COND(&s_runtime.cond);
      }
      break;
    }

    if (s_runtime.pending_index == -1 && s_runtime.used[active] > 0U) {
      s_runtime.pending_index = active;
      s_runtime.active_index = 1 - active;
      _nei_log_ensure_active_not_consuming(&s_runtime);
  _NEI_LOG_SIGNAL_COND(&s_runtime.cond);
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

#pragma endregion

#pragma region consumer

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
      _NEI_LOG_SIGNAL_COND(&rt->cond);
      LeaveCriticalSection(&rt->mutex);
    #else
      pthread_mutex_lock(&rt->mutex);
      rt->used[consume_index] = 0U;
      rt->consuming_index = -1;
      _NEI_LOG_SIGNAL_COND(&rt->cond);
      pthread_mutex_unlock(&rt->mutex);
    #endif
  }

#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

#pragma endregion
