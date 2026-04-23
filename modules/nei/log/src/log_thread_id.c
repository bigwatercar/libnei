#include "log_internal.h"

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

void _nei_log_tls_thread_id_cstr(const char **out_str, size_t *out_len) {
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

int _nei_log_config_wants_thread_id(nei_log_config_handle_t config_handle) {
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

void _nei_log_header_fill_thread_id(nei_log_event_header_st *header, nei_log_config_handle_t config_handle) {
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
