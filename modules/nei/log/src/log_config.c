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
  cfg->log_to_console = 0;
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
  _nei_log_config_lock_write();
  _nei_log_ensure_config_table_initialized();
  if (_nei_log_slot_from_handle(handle, &slot) != 0 || s_config_used[slot] == 0U) {
    _nei_log_config_unlock_write();
    return;
  }
  if (slot == 0U) {
    _nei_log_reset_default_config();
    _nei_log_config_snapshot_bump();
    _nei_log_config_unlock_write();
    return;
  }
  s_config_used[slot] = 0U;
  s_config_ptrs[slot] = NULL;
  _nei_log_config_snapshot_bump();
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
    _nei_log_config_snapshot_bump();
  }
  _nei_log_config_unlock_write();
  return cfg;
}

#pragma endregion
