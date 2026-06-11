#include "log_internal.h"

#pragma region config table

/* Global configuration table */
nei_log_config_st *s_config_ptrs[_NEI_LOG_MAX_CONFIGS];
nei_log_config_st s_custom_configs[_NEI_LOG_MAX_CONFIGS];
uint8_t s_config_used[_NEI_LOG_MAX_CONFIGS];
int s_config_table_initialized = 0;
#if defined(_WIN32)
volatile LONGLONG s_config_snapshot = 1;
#else
uint64_t s_config_snapshot = 1;
#endif
#if defined(_WIN32)
SRWLOCK s_config_lock = SRWLOCK_INIT;
#else
pthread_rwlock_t s_config_lock = PTHREAD_RWLOCK_INITIALIZER;
#endif

#pragma endregion

#pragma region implementation

void _nei_log_ensure_config_table_initialized(void) {
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

void _nei_log_fill_default_config(nei_log_config_st *cfg) {
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
  cfg->immediate_crash_on_fatal = 0; /* Disabled by default */
  cfg->timestamp_style = NEI_LOG_TIMESTAMP_STYLE_DEFAULT;
}

void _nei_log_reset_default_config(void) {
  _nei_log_fill_default_config(&s_custom_configs[0]);
}

void _nei_log_config_lock_read(void) {
#if defined(_WIN32)
  AcquireSRWLockShared(&s_config_lock);
#else
  (void)pthread_rwlock_rdlock(&s_config_lock);
#endif
}

void _nei_log_config_lock_write(void) {
#if defined(_WIN32)
  AcquireSRWLockExclusive(&s_config_lock);
#else
  (void)pthread_rwlock_wrlock(&s_config_lock);
#endif
}

void _nei_log_config_unlock_read(void) {
#if defined(_WIN32)
  ReleaseSRWLockShared(&s_config_lock);
#else
  (void)pthread_rwlock_unlock(&s_config_lock);
#endif
}

void _nei_log_config_unlock_write(void) {
#if defined(_WIN32)
  ReleaseSRWLockExclusive(&s_config_lock);
#else
  (void)pthread_rwlock_unlock(&s_config_lock);
#endif
}

nei_log_config_handle_t _nei_log_make_handle_from_slot(size_t slot) {
  if (slot >= _NEI_LOG_MAX_CONFIGS) {
    return NEI_LOG_INVALID_CONFIG_HANDLE;
  }
  return (nei_log_config_handle_t)(slot + 1U);
}

int _nei_log_slot_from_handle(nei_log_config_handle_t handle, size_t *out_slot) {
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

uint64_t _nei_log_config_snapshot_load(void) {
#if defined(_WIN32)
  return (uint64_t)InterlockedCompareExchange64(&s_config_snapshot, 0, 0);
#else
  return __atomic_load_n(&s_config_snapshot, __ATOMIC_ACQUIRE);
#endif
}

void _nei_log_config_snapshot_bump(void) {
#if defined(_WIN32)
  (void)InterlockedIncrement64(&s_config_snapshot);
#else
  (void)__atomic_add_fetch(&s_config_snapshot, 1U, __ATOMIC_RELEASE);
#endif
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
  _nei_log_config_snapshot_bump();
  if (out_handle != NULL) {
    *out_handle = _nei_log_make_handle_from_slot(free_slot);
  }

  _nei_log_config_unlock_write();
  return 0;
}

void nei_log_remove_config(nei_log_config_handle_t handle) {
  size_t slot = 0U;
  nei_log_sink_st *sinks_to_release[NEI_LOG_MAX_SINKS_OF_CONFIG];
  size_t num_sinks = 0;

  /* Flush once before taking the lock: drain any events already in the ring
   * buffer so the consumer is idle when we modify the config table. */
  nei_log_flush();

  _nei_log_config_lock_write();
  _nei_log_ensure_config_table_initialized();
  if (_nei_log_slot_from_handle(handle, &slot) != 0 || s_config_used[slot] == 0U) {
    _nei_log_config_unlock_write();
    return;
  }

  /* Save sink pointers with release callbacks before clearing the slot.
   * This prevents races with slot reuse and ensures sinks are released
   * outside the write lock (release() may perform I/O or take other locks). */
  {
    const nei_log_config_st *cfg = s_config_ptrs[slot];
    if (cfg != NULL) {
      size_t i;
      for (i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
        if (cfg->sinks[i] != NULL && cfg->sinks[i]->release != NULL) {
          sinks_to_release[num_sinks++] = cfg->sinks[i];
        }
      }
    }
  }

  if (slot == 0U) {
    _nei_log_reset_default_config();
  } else {
    s_config_used[slot] = 0U;
    s_config_ptrs[slot] = NULL;
  }
  _nei_log_config_snapshot_bump();
  _nei_log_config_unlock_write();

  if (num_sinks > 0U) {
    /* Flush again after the snapshot bump: drain any events that were
     * enqueued between the first flush and the snapshot update.  The
     * consumer's resolve_config_cached() will now see the new snapshot
     * and find the config removed, so it will skip emit_message() for
     * these events — guaranteeing no consumer is accessing the sinks
     * when we call release(). */
    nei_log_flush();

    /* Release sinks: at this point the config is unpublished and the
     * consumer has drained all events, so it is safe to release sink
     * resources (close handles, free memory, etc.). */
    size_t i;
    for (i = 0; i < num_sinks; ++i) {
      sinks_to_release[i]->release(sinks_to_release[i]);
    }
  }
}

void nei_log_update_config(void) {
  _nei_log_config_snapshot_bump();
}

void nei_log_shutdown(void) {
  size_t slot;

  /* Remove every active configuration.  nei_log_remove_config handles
   * flush + snapshot + sink release internally for each slot. */
  for (slot = 0U; slot < _NEI_LOG_MAX_CONFIGS; ++slot) {
    nei_log_remove_config(_nei_log_make_handle_from_slot(slot));
  }

  /* Stop the consumer thread and destroy runtime resources. */
  _nei_log_shutdown_runtime();
}

int nei_log_add_sink(nei_log_config_st *config, nei_log_sink_st *sink) {
  size_t i;

  if (config == NULL || sink == NULL) {
    return -1;
  }

  for (i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
    if (config->sinks[i] == NULL) {
      config->sinks[i] = sink;
      return 0;
    }
  }

  return -1; /* Sink array is full */
}

int nei_log_remove_sink(nei_log_config_st *config, nei_log_sink_st *sink) {
  size_t i;
  size_t found = (size_t)-1;

  if (config == NULL || sink == NULL) {
    return -1;
  }

  /* Find the sink in the array. */
  for (i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
    if (config->sinks[i] == sink) {
      found = i;
      break;
    }
    if (config->sinks[i] == NULL) {
      break; /* End of active sinks */
    }
  }

  if (found == (size_t)-1) {
    return -1;
  }

  /* Compact: shift remaining sinks left to keep the array contiguous. */
  for (i = found; i < NEI_LOG_MAX_SINKS_OF_CONFIG - 1U; ++i) {
    config->sinks[i] = config->sinks[i + 1];
    if (config->sinks[i] == NULL) {
      break;
    }
  }
  config->sinks[NEI_LOG_MAX_SINKS_OF_CONFIG - 1U] = NULL;

  return 0;
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

  _nei_log_config_lock_read();
  _nei_log_ensure_config_table_initialized();
  cfg = s_config_ptrs[0];
  _nei_log_config_unlock_read();

  if (cfg != NULL) {
    return cfg;
  }

  /* Slow path: first access initializes the default config under a write
   * lock, but the common case (post-init) only takes the read lock. */
  _nei_log_config_lock_write();
  _nei_log_ensure_config_table_initialized();
  if (s_config_ptrs[0] == NULL) {
    s_config_used[0] = 1U;
    s_config_ptrs[0] = &s_custom_configs[0];
    _nei_log_fill_default_config(s_config_ptrs[0]);
    _nei_log_config_snapshot_bump();
  }
  cfg = s_config_ptrs[0];
  _nei_log_config_unlock_write();
  return cfg;
}

#pragma endregion
