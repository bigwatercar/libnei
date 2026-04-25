#include "log_internal.h"

#pragma region runtime

/* Forward declaration */
#if defined(_WIN32)
static DWORD WINAPI _nei_log_consumer_thread(LPVOID arg);
#else
static void *_nei_log_consumer_thread(void *arg);
#endif

/* Forward declaration */
static void _nei_log_process_events(const uint8_t *buf, size_t size);

nei_log_runtime_st s_runtime = {
  .stop_requested = 0,
  .initialized = 0,
};

static uint32_t s_runtime_init_count = 0U;

void _nei_log_signal_consumer_if_sleeping(void) {
  if (_NEI_LOG_ATOMIC_LOAD32(&s_runtime.consumer_sleeping) == 0U) {
    return;
  }

#if defined(_WIN32)
  EnterCriticalSection(&s_runtime.mutex);
  if (_NEI_LOG_ATOMIC_LOAD32(&s_runtime.consumer_sleeping) != 0U) {
    _NEI_LOG_SIGNAL_COND(&s_runtime.cond);
  }
  LeaveCriticalSection(&s_runtime.mutex);
#else
  pthread_mutex_lock(&s_runtime.mutex);
  if (_NEI_LOG_ATOMIC_LOAD32(&s_runtime.consumer_sleeping) != 0U) {
    pthread_cond_signal(&s_runtime.cond);
  }
  pthread_mutex_unlock(&s_runtime.mutex);
#endif
}

static void _nei_log_update_ring_hwm(uint64_t depth) {
#if defined(_WIN32)
  for (;;) {
    uint64_t old = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.stat_ring_high_watermark);
    if (depth <= old) {
      return;
    }
    if ((uint64_t)InterlockedCompareExchange64((volatile LONGLONG *)&s_runtime.stat_ring_high_watermark,
                                               (LONGLONG)depth,
                                               (LONGLONG)old) == old) {
      return;
    }
  }
#else
  for (;;) {
    uint64_t old = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.stat_ring_high_watermark);
    if (depth <= old) {
      return;
    }
    if (__atomic_compare_exchange_n(&s_runtime.stat_ring_high_watermark,
                                    &old,
                                    depth,
                                    0,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
      return;
    }
  }
#endif
}

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

int _nei_log_get_perf_stats_for_test(nei_log_perf_stats_st *out_stats) {
  if (out_stats == NULL) {
    return -1;
  }
  out_stats->producer_spin_loops = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.stat_producer_spin_loops);
  out_stats->flush_wait_loops = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.stat_flush_wait_loops);
  out_stats->consumer_wakeups = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.stat_consumer_wakeups);
  out_stats->ring_high_watermark = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.stat_ring_high_watermark);
  return 0;
}

void _nei_log_reset_perf_stats_for_test(void) {
  _NEI_LOG_ATOMIC_STORE64(&s_runtime.stat_producer_spin_loops, 0U);
  _NEI_LOG_ATOMIC_STORE64(&s_runtime.stat_flush_wait_loops, 0U);
  _NEI_LOG_ATOMIC_STORE64(&s_runtime.stat_consumer_wakeups, 0U);
  _NEI_LOG_ATOMIC_STORE64(&s_runtime.stat_ring_high_watermark, 0U);
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
static uint64_t _nei_log_drain_ring(nei_log_ring_st *ring) {
  uint64_t drained = 0U;
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
    drained += 1U;
  }
  return drained;
}

static void _nei_log_notify_waiters_after_drain(nei_log_runtime_st *rt) {
  const uint64_t backlog = _NEI_LOG_ATOMIC_LOAD64(&rt->ring.write_pos) - _NEI_LOG_ATOMIC_LOAD64(&rt->ring.consumer_pos);
#if defined(_WIN32)
  EnterCriticalSection(&rt->mutex);
  if (backlog > 8U) {
    _NEI_LOG_BROADCAST_COND(&rt->cond);
  } else {
    _NEI_LOG_SIGNAL_COND(&rt->cond);
  }
  LeaveCriticalSection(&rt->mutex);
#else
  pthread_mutex_lock(&rt->mutex);
  if (backlog > 8U) {
    _NEI_LOG_BROADCAST_COND(&rt->cond);
  } else {
    _NEI_LOG_SIGNAL_COND(&rt->cond);
  }
  pthread_mutex_unlock(&rt->mutex);
#endif
}

int _nei_log_enqueue_event(const uint8_t *event, size_t len) {
  uint64_t pos;
  uint64_t spins = 0U;
  uint64_t consumer_pos_snapshot;
  uint64_t depth_after_commit;
  uint32_t idx;
  nei_log_ring_slot_st *slot;

  if (event == NULL || len == 0U || len > _NEI_LOG_EVENT_BUFFER_SIZE || !s_runtime.initialized) {
    return -1;
  }

  /* Atomically reserve one slot (fetch-add, no mutex). */
  pos = _NEI_LOG_ATOMIC_FETCH_ADD64(&s_runtime.ring.write_pos, 1U);
  idx = (uint32_t)(pos % (uint64_t)_NEI_LOG_RING_SLOTS);
  slot = &s_runtime.ring.slots[idx];
  consumer_pos_snapshot = _NEI_LOG_ATOMIC_LOAD64(&s_runtime.ring.consumer_pos);
  depth_after_commit = (pos + 1U) - consumer_pos_snapshot;
  _nei_log_update_ring_hwm(depth_after_commit);

  /* Spin-wait for the reserved slot to be released by the consumer.
   * IMPORTANT: once write_pos is incremented we must eventually publish this
   * exact sequence number. Dropping here would leave a "hole" and stall flush
   * forever because consumer_pos can only advance in order. */
  while (_NEI_LOG_ATOMIC_LOAD32(&slot->state) != 0U) {
    spins += 1U;
    if (spins <= _NEI_LOG_RING_WAIT_RELAX_ITERS) {
      _NEI_LOG_CPU_YIELD();
      continue;
    }
    if (spins <= _NEI_LOG_RING_WAIT_YIELD_ITERS) {
      _NEI_LOG_THREAD_YIELD();
      continue;
    }

#if defined(_WIN32)
    EnterCriticalSection(&s_runtime.mutex);
    while (_NEI_LOG_ATOMIC_LOAD32(&slot->state) != 0U && !s_runtime.stop_requested) {
      SleepConditionVariableCS(&s_runtime.cond, &s_runtime.mutex, INFINITE);
    }
    LeaveCriticalSection(&s_runtime.mutex);
#else
    pthread_mutex_lock(&s_runtime.mutex);
    while (_NEI_LOG_ATOMIC_LOAD32(&slot->state) != 0U && !s_runtime.stop_requested) {
      pthread_cond_wait(&s_runtime.cond, &s_runtime.mutex);
    }
    pthread_mutex_unlock(&s_runtime.mutex);
#endif
  }

  if (spins > 0U) {
    (void)_NEI_LOG_ATOMIC_FETCH_ADD64(&s_runtime.stat_producer_spin_loops, spins);
  }

  /* Write the serialized event, then publish with a store-release. */
  slot->size = (uint32_t)len;
  memcpy(slot->data, event, len);
  _NEI_LOG_ATOMIC_STORE32(&slot->state, 1U);

  /* Fast path: only wake the consumer when it has actually gone to sleep. */
  _nei_log_signal_consumer_if_sleeping();
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
  uint32_t idle_spin = 0U;
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
    while (!_nei_log_ring_has_ready_slot(&rt->ring) && !rt->stop_requested) {
      _NEI_LOG_ATOMIC_STORE32(&rt->consumer_sleeping, 1U);
      if (_nei_log_ring_has_ready_slot(&rt->ring) || rt->stop_requested) {
        _NEI_LOG_ATOMIC_STORE32(&rt->consumer_sleeping, 0U);
        break;
      }
      SleepConditionVariableCS(&rt->cond, &rt->mutex, INFINITE);
      _NEI_LOG_ATOMIC_STORE32(&rt->consumer_sleeping, 0U);
      (void)_NEI_LOG_ATOMIC_FETCH_ADD64(&rt->stat_consumer_wakeups, 1U);
    }
    if (rt->stop_requested && !_nei_log_ring_has_ready_slot(&rt->ring)) {
      _NEI_LOG_ATOMIC_STORE32(&rt->consumer_sleeping, 0U);
      LeaveCriticalSection(&rt->mutex);
      (void)_nei_log_drain_ring(&rt->ring); /* final drain */
      break;
    }
    LeaveCriticalSection(&rt->mutex);
    {
      uint64_t drained = _nei_log_drain_ring(&rt->ring);
      if (drained > 0U) {
        /* Adaptive idle spin: longer in quiet/sync mode (small drain batch) so the
         * consumer stays hot between per-call flushes; shorter in burst/async mode
         * to avoid burning CPU between large drain batches. */
        uint32_t adaptive_iters;
        if (drained >= 16U) {
          adaptive_iters = _NEI_LOG_CONSUMER_IDLE_SPIN_ITERS / 4U; /* 128: burst */
        } else if (drained >= 4U) {
          adaptive_iters = _NEI_LOG_CONSUMER_IDLE_SPIN_ITERS / 2U; /* 256: medium */
        } else {
          adaptive_iters = _NEI_LOG_CONSUMER_IDLE_SPIN_ITERS;       /* 512: sync/quiet */
        }
        _nei_log_notify_waiters_after_drain(rt);
        for (idle_spin = 0U; idle_spin < adaptive_iters; ++idle_spin) {
          if (_nei_log_ring_has_ready_slot(&rt->ring) || rt->stop_requested) {
            break;
          }
          /* SwitchToThread every 64th iter (less OS overhead than every 32nd). */
          if ((idle_spin & 63U) == 63U) {
            _NEI_LOG_THREAD_YIELD();
          } else {
            _NEI_LOG_CPU_YIELD();
          }
        }
        EnterCriticalSection(&rt->mutex);
        continue;
      }
    }
    EnterCriticalSection(&rt->mutex);
  }
#else
  pthread_mutex_lock(&rt->mutex);
  for (;;) {
    while (!_nei_log_ring_has_ready_slot(&rt->ring) && !rt->stop_requested) {
      _NEI_LOG_ATOMIC_STORE32(&rt->consumer_sleeping, 1U);
      if (_nei_log_ring_has_ready_slot(&rt->ring) || rt->stop_requested) {
        _NEI_LOG_ATOMIC_STORE32(&rt->consumer_sleeping, 0U);
        break;
      }
      pthread_cond_wait(&rt->cond, &rt->mutex);
      _NEI_LOG_ATOMIC_STORE32(&rt->consumer_sleeping, 0U);
      (void)_NEI_LOG_ATOMIC_FETCH_ADD64(&rt->stat_consumer_wakeups, 1U);
    }
    if (rt->stop_requested && !_nei_log_ring_has_ready_slot(&rt->ring)) {
      _NEI_LOG_ATOMIC_STORE32(&rt->consumer_sleeping, 0U);
      pthread_mutex_unlock(&rt->mutex);
      (void)_nei_log_drain_ring(&rt->ring); /* final drain */
      break;
    }
    pthread_mutex_unlock(&rt->mutex);
    {
      uint64_t drained = _nei_log_drain_ring(&rt->ring);
      if (drained > 0U) {
        uint32_t adaptive_iters;
        if (drained >= 16U) {
          adaptive_iters = _NEI_LOG_CONSUMER_IDLE_SPIN_ITERS / 4U;
        } else if (drained >= 4U) {
          adaptive_iters = _NEI_LOG_CONSUMER_IDLE_SPIN_ITERS / 2U;
        } else {
          adaptive_iters = _NEI_LOG_CONSUMER_IDLE_SPIN_ITERS;
        }
        _nei_log_notify_waiters_after_drain(rt);
        for (idle_spin = 0U; idle_spin < adaptive_iters; ++idle_spin) {
          if (_nei_log_ring_has_ready_slot(&rt->ring) || rt->stop_requested) {
            break;
          }
          if ((idle_spin & 63U) == 63U) {
            _NEI_LOG_THREAD_YIELD();
          } else {
            _NEI_LOG_CPU_YIELD();
          }
        }
        pthread_mutex_lock(&rt->mutex);
        continue;
      }
    }
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
