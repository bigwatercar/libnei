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

static uint32_t s_runtime_init_count = 0U;

#if defined(_WIN32)
static INIT_ONCE s_runtime_init_once = INIT_ONCE_STATIC_INIT;
static DWORD s_runtime_init_error = ERROR_SUCCESS;

static BOOL CALLBACK _nei_log_runtime_init_once_callback(PINIT_ONCE init_once, PVOID param, PVOID *context) {
  (void)init_once;
  (void)param;
  (void)context;

  InitializeCriticalSection(&s_runtime.mutex);
  InitializeConditionVariable(&s_runtime.cond);
  s_runtime.thread = CreateThread(NULL, 0, _nei_log_consumer_thread, &s_runtime, 0, NULL);
  if (s_runtime.thread == NULL) {
    s_runtime_init_error = GetLastError();
    DeleteCriticalSection(&s_runtime.mutex);
    return FALSE;
  }

  s_runtime.stop_requested = 0;
  s_runtime.initialized = 1;
  s_runtime_init_count += 1U;
  (void)atexit(_nei_log_shutdown_runtime);
  s_runtime_init_error = ERROR_SUCCESS;
  return TRUE;
}
#else
static pthread_once_t s_runtime_init_once = PTHREAD_ONCE_INIT;
static int s_runtime_init_error = 0;

static void _nei_log_runtime_init_once_callback(void) {
  int rc = 0;

  rc = pthread_mutex_init(&s_runtime.mutex, NULL);
  if (rc != 0) {
    s_runtime_init_error = rc;
    return;
  }
  rc = pthread_cond_init(&s_runtime.cond, NULL);
  if (rc != 0) {
    s_runtime_init_error = rc;
    pthread_mutex_destroy(&s_runtime.mutex);
    return;
  }
  rc = pthread_create(&s_runtime.thread, NULL, _nei_log_consumer_thread, &s_runtime);
  if (rc != 0) {
    s_runtime_init_error = rc;
    pthread_cond_destroy(&s_runtime.cond);
    pthread_mutex_destroy(&s_runtime.mutex);
    return;
  }

  s_runtime.stop_requested = 0;
  s_runtime.initialized = 1;
  s_runtime_init_count += 1U;
  (void)atexit(_nei_log_shutdown_runtime);
  s_runtime_init_error = 0;
}
#endif

int _nei_log_ensure_runtime_initialized(void) {
  if (s_runtime.initialized) {
    return 0;
  }

#if defined(_WIN32)
  if (!InitOnceExecuteOnce(&s_runtime_init_once, _nei_log_runtime_init_once_callback, NULL, NULL)) {
    return -1;
  }
#else
  if (pthread_once(&s_runtime_init_once, _nei_log_runtime_init_once_callback) != 0) {
    return -1;
  }
#endif

  if (!s_runtime.initialized || s_runtime_init_error != 0) {
    return -1;
  }
  return 0;
}

uint32_t _nei_log_get_runtime_init_count_for_test(void) {
  return s_runtime_init_count;
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

void _nei_log_wait_until_buffer_not_consuming(nei_log_runtime_st *rt, int buffer_index) {
  while (rt->consuming_index >= 0 && buffer_index == rt->consuming_index) {
#if defined(_WIN32)
    SleepConditionVariableCS(&rt->cond, &rt->mutex, INFINITE);
#else
    pthread_cond_wait(&rt->cond, &rt->mutex);
#endif
  }
}

void _nei_log_publish_pending_buffer(nei_log_runtime_st *rt, int pending_index) {
  const int next_active = 1 - pending_index;

  _nei_log_wait_until_buffer_not_consuming(rt, next_active);
  rt->pending_index = pending_index;
  rt->active_index = next_active;
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
        _nei_log_publish_pending_buffer(&s_runtime, active);
        _NEI_LOG_SIGNAL_COND(&s_runtime.cond);
      }
      break;
    }

    if (s_runtime.pending_index == -1 && s_runtime.used[active] > 0U) {
      _nei_log_publish_pending_buffer(&s_runtime, active);
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

typedef struct _nei_log_consumer_cfg_cache_st {
  nei_log_config_handle_t handle;
  uint64_t snapshot;
  const nei_log_config_st *cfg;
} nei_log_consumer_cfg_cache_st;

static _NEI_LOG_TLS nei_log_consumer_cfg_cache_st s_tls_consumer_cfg_cache;

static const nei_log_config_st *_nei_log_resolve_config_cached(nei_log_config_handle_t handle) {
  size_t slot = 0U;
  const nei_log_config_st *cfg = NULL;
  const uint64_t snapshot = _nei_log_config_snapshot_load();

  if (s_tls_consumer_cfg_cache.handle == handle && s_tls_consumer_cfg_cache.snapshot == snapshot) {
    return s_tls_consumer_cfg_cache.cfg;
  }

  _nei_log_config_lock_read();
  _nei_log_ensure_config_table_initialized();
  if (_nei_log_slot_from_handle(handle, &slot) == 0 && s_config_used[slot] != 0U) {
    cfg = s_config_ptrs[slot];
  }
  _nei_log_config_unlock_read();

  s_tls_consumer_cfg_cache.handle = handle;
  s_tls_consumer_cfg_cache.snapshot = snapshot;
  s_tls_consumer_cfg_cache.cfg = cfg;
  return cfg;
}

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
    config = _nei_log_resolve_config_cached(header->config_handle);
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
        _nei_log_publish_pending_buffer(rt, rt->active_index);
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
        _nei_log_publish_pending_buffer(rt, rt->active_index);
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
