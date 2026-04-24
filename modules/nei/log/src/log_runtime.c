#include "log_internal.h"

#pragma region runtime

/* Forward declaration */
#if defined(_WIN32)
static DWORD WINAPI _nei_log_consumer_thread(LPVOID arg);
#else
static void *_nei_log_consumer_thread(void *arg);
#endif

nei_log_runtime_st s_runtime = {
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
  (void)rt;
  (void)buffer_index;
  /* Intentionally empty: retained only so existing callers compile during transition. */
}

void _nei_log_publish_pending_buffer(nei_log_runtime_st *rt, int pending_index) {
  (void)rt;
  (void)pending_index;
  /* Intentionally empty: replaced by the lock-free ring buffer. */
}

/* ── Ring-buffer helpers (consumer-side) ──────────────────────────────────── */

/* Forward declaration — defined in the consumer region below. */
static void _nei_log_process_events(const uint8_t *buf, size_t size);

/** Returns non-zero if the next slot the consumer should read is committed. */
static int _nei_log_ring_has_ready_slot(nei_log_ring_st *ring) {
  uint64_t cpos = _NEI_LOG_ATOMIC_LOAD64(&ring->consumer_pos);
  uint64_t wpos = _NEI_LOG_ATOMIC_LOAD64(&ring->write_pos);
  if (cpos >= wpos) {
    return 0;
  }
  return _NEI_LOG_ATOMIC_LOAD32(&ring->slots[(uint32_t)(cpos % _NEI_LOG_RING_SLOTS)].state) != 0U;
}

/** Consume all committed slots in order; stops at the first uncommitted one. */
static void _nei_log_drain_ring(nei_log_ring_st *ring) {
  for (;;) {
    uint64_t cpos = _NEI_LOG_ATOMIC_LOAD64(&ring->consumer_pos);
    uint32_t idx = (uint32_t)(cpos % (uint64_t)_NEI_LOG_RING_SLOTS);
    nei_log_ring_slot_st *slot = &ring->slots[idx];
    if (_NEI_LOG_ATOMIC_LOAD32(&slot->state) != 1U) {
      break; /* next slot not yet committed */
    }
    _nei_log_process_events(slot->data, (size_t)slot->size);
    /* Release slot so producers may reuse it. */
    _NEI_LOG_ATOMIC_STORE32(&slot->state, 0U);
    _NEI_LOG_ATOMIC_STORE64(&ring->consumer_pos, cpos + 1U);
  }
}

int _nei_log_enqueue_event(const uint8_t *event, size_t len) {
  uint64_t pos;
  uint32_t idx;
  nei_log_ring_slot_st *slot;
  int spins;

  if (event == NULL || len == 0U || len > _NEI_LOG_EVENT_BUFFER_SIZE || !s_runtime.initialized) {
    return -1;
  }

  /* Atomically reserve one slot (fetch-add, no mutex). */
  pos = _NEI_LOG_ATOMIC_FETCH_ADD64(&s_runtime.ring.write_pos, 1U);
  idx = (uint32_t)(pos % (uint64_t)_NEI_LOG_RING_SLOTS);
  slot = &s_runtime.ring.slots[idx];

  /* Spin-wait for the slot to be empty (ring-full back-pressure).
   * With 256 slots the ring is far deeper than typical burst depth.
   * After _NEI_LOG_RING_FULL_SPINS iterations, drop rather than block. */
  spins = 0;
  while (_NEI_LOG_ATOMIC_LOAD32(&slot->state) != 0U) {
    if (++spins > _NEI_LOG_RING_FULL_SPINS) {
      return -1; /* ring full — drop this event */
    }
    _NEI_LOG_CPU_YIELD();
  }

  /* Write the serialized event, then publish with a store-release. */
  slot->size = (uint32_t)len;
  memcpy(slot->data, event, len);
  _NEI_LOG_ATOMIC_STORE32(&slot->state, 1U);

  /* Wake the consumer.  WakeConditionVariable (Windows) may be called without
   * owning the critical section.  pthread_cond_signal (POSIX) is safe here
   * because the consumer uses a 10 ms timeout to handle any lost wakeup. */
  _NEI_LOG_SIGNAL_COND(&s_runtime.cond);
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

#if defined(_WIN32)
  EnterCriticalSection(&rt->mutex);
  for (;;) {
    /* Sleep until a slot is committed, stop is requested, or 10 ms elapses.
     * The timeout handles the rare case where a producer's WakeConditionVariable
     * fires before the consumer has entered SleepConditionVariableCS. */
    while (!_nei_log_ring_has_ready_slot(&rt->ring) && !rt->stop_requested) {
      SleepConditionVariableCS(&rt->cond, &rt->mutex, 10 /* ms */);
    }
    if (rt->stop_requested && !_nei_log_ring_has_ready_slot(&rt->ring)) {
      LeaveCriticalSection(&rt->mutex);
      _nei_log_drain_ring(&rt->ring); /* final drain */
      break;
    }
    LeaveCriticalSection(&rt->mutex);
    _nei_log_drain_ring(&rt->ring);
    EnterCriticalSection(&rt->mutex);
  }
#else
  pthread_mutex_lock(&rt->mutex);
  for (;;) {
    struct timespec ts;
    /* 10 ms timeout: handles lost wakeup when producer signals without lock. */
    while (!_nei_log_ring_has_ready_slot(&rt->ring) && !rt->stop_requested) {
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 10000000L; /* +10 ms */
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&rt->cond, &rt->mutex, &ts);
    }
    if (rt->stop_requested && !_nei_log_ring_has_ready_slot(&rt->ring)) {
      pthread_mutex_unlock(&rt->mutex);
      _nei_log_drain_ring(&rt->ring); /* final drain */
      break;
    }
    pthread_mutex_unlock(&rt->mutex);
    _nei_log_drain_ring(&rt->ring);
    pthread_mutex_lock(&rt->mutex);
  }
#endif

#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

#pragma endregion
