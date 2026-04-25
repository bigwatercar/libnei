#include "log_internal.h"

#pragma region serialization helpers

static inline size_t _nei_log_align_up_8(size_t n) {
  return (n + 7U) & ~(size_t)7U;
}

static const char *_nei_log_wstr_to_mbs_or_placeholder(const wchar_t *ws, char *out, size_t out_size);

typedef struct _nei_log_fmt_plan_op_st {
  uint8_t payload_type;
  uint8_t conv_lm;
} _nei_log_fmt_plan_op_st;

#define _NEI_LOG_FMT_PLAN_MAX_OPS 64U

typedef struct _nei_log_fmt_plan_cache_st {
  const char *fmt;
  uint16_t op_count;
  uint8_t ready;
  uint8_t reject;
  _nei_log_fmt_plan_op_st ops[_NEI_LOG_FMT_PLAN_MAX_OPS];
} _nei_log_fmt_plan_cache_st;

static _NEI_LOG_TLS _nei_log_fmt_plan_cache_st s_tls_fmt_plan_cache;

static int _nei_log_payload_write_u8(uint8_t *out, size_t out_cap, size_t *used, uint8_t value) {
  if (out == NULL || used == NULL || *used + 1U > out_cap) {
    return -1;
  }
  out[*used] = value;
  *used += 1U;
  return 0;
}

static int _nei_log_payload_write_u16(uint8_t *out, size_t out_cap, size_t *used, uint16_t value) {
  if (out == NULL || used == NULL || *used + sizeof(uint16_t) > out_cap) {
    return -1;
  }
  memcpy(out + *used, &value, sizeof(uint16_t));
  *used += sizeof(uint16_t);
  return 0;
}

static int _nei_log_payload_write_bytes(uint8_t *out, size_t out_cap, size_t *used, const void *src, size_t len) {
  if (out == NULL || used == NULL || src == NULL || *used + len > out_cap) {
    return -1;
  }
  memcpy(out + *used, src, len);
  *used += len;
  return 0;
}

static int _nei_log_payload_write_padded_zero(uint8_t *out, size_t out_cap, size_t *used, size_t target_size) {
  if (out == NULL || used == NULL || target_size < *used || target_size > out_cap) {
    return -1;
  }
  while (*used < target_size) {
    out[*used] = 0;
    *used += 1U;
  }
  return 0;
}

#if defined(_WIN32)
#define _NEI_LOG_VA_PARAM va_list *
#define _NEI_LOG_VA_PASS(ap) (&(ap))
#define _NEI_LOG_VA_ARG(ap, type) va_arg(*(ap), type)
#else
#define _NEI_LOG_VA_PARAM va_list
#define _NEI_LOG_VA_PASS(ap) (ap)
#define _NEI_LOG_VA_ARG(ap, type) va_arg((ap), type)
#endif

static int _nei_log_payload_emit_arg(uint8_t *out,
                                     size_t out_cap,
                                     size_t *used,
                                     uint8_t payload_type,
                                     uint8_t conv_lm,
                                     _NEI_LOG_VA_PARAM args) {
  if (_nei_log_payload_write_u8(out, out_cap, used, payload_type) != 0) {
    return -1;
  }

  switch (payload_type) {
  case _NEI_LOG_PAYLOAD_I32: {
    int32_t v = (int32_t)_NEI_LOG_VA_ARG(args, int);
    return _nei_log_payload_write_bytes(out, out_cap, used, &v, sizeof(v));
  }
  case _NEI_LOG_PAYLOAD_U32: {
    uint32_t v = (uint32_t)_NEI_LOG_VA_ARG(args, unsigned int);
    return _nei_log_payload_write_bytes(out, out_cap, used, &v, sizeof(v));
  }
  case _NEI_LOG_PAYLOAD_I64: {
    int64_t v64 = 0;
    if (conv_lm == 3U) {
      v64 = (int64_t)_NEI_LOG_VA_ARG(args, long);
    } else if (conv_lm == 4U) {
      v64 = (int64_t)_NEI_LOG_VA_ARG(args, long long);
    } else if (conv_lm == 5U) {
      v64 = (int64_t)_NEI_LOG_VA_ARG(args, intmax_t);
    } else if (conv_lm == 6U) {
      v64 = (int64_t)_NEI_LOG_VA_ARG(args, ptrdiff_t);
    } else if (conv_lm == 7U) {
      v64 = (int64_t)_NEI_LOG_VA_ARG(args, ptrdiff_t);
    } else {
      v64 = (int64_t)_NEI_LOG_VA_ARG(args, long long);
    }
    return _nei_log_payload_write_bytes(out, out_cap, used, &v64, sizeof(v64));
  }
  case _NEI_LOG_PAYLOAD_U64: {
    uint64_t v64 = 0;
    if (conv_lm == 3U) {
      v64 = (uint64_t)_NEI_LOG_VA_ARG(args, unsigned long);
    } else if (conv_lm == 4U) {
      v64 = (uint64_t)_NEI_LOG_VA_ARG(args, unsigned long long);
    } else if (conv_lm == 5U) {
      v64 = (uint64_t)_NEI_LOG_VA_ARG(args, uintmax_t);
    } else if (conv_lm == 6U || conv_lm == 7U) {
      v64 = (uint64_t)_NEI_LOG_VA_ARG(args, size_t);
    } else {
      v64 = (uint64_t)_NEI_LOG_VA_ARG(args, unsigned long long);
    }
    return _nei_log_payload_write_bytes(out, out_cap, used, &v64, sizeof(v64));
  }
  case _NEI_LOG_PAYLOAD_DOUBLE: {
    double v = _NEI_LOG_VA_ARG(args, double);
    return _nei_log_payload_write_bytes(out, out_cap, used, &v, sizeof(v));
  }
  case _NEI_LOG_PAYLOAD_LONGDOUBLE: {
    long double ld = _NEI_LOG_VA_ARG(args, long double);
    uint8_t raw[_NEI_LOG_LONGDOUBLE_STORAGE];
    memset(raw, 0, sizeof(raw));
    memcpy(raw, &ld, sizeof(ld) < sizeof(raw) ? sizeof(ld) : sizeof(raw));
    return _nei_log_payload_write_bytes(out, out_cap, used, raw, sizeof(raw));
  }
  case _NEI_LOG_PAYLOAD_CHAR: {
    char ch = (char)_NEI_LOG_VA_ARG(args, int);
    return _nei_log_payload_write_bytes(out, out_cap, used, &ch, sizeof(ch));
  }
  case _NEI_LOG_PAYLOAD_PTR: {
    uintptr_t raw = (uintptr_t)_NEI_LOG_VA_ARG(args, const void *);
    return _nei_log_payload_write_bytes(out, out_cap, used, &raw, sizeof(raw));
  }
  case _NEI_LOG_PAYLOAD_CSTR: {
    const char *s = NULL;
    char ws_buf[_NEI_LOG_MAX_STRING_COPY + 1U];
    uint16_t len16;
    size_t len;
    if (conv_lm == 3U) {
      const wchar_t *ws = _NEI_LOG_VA_ARG(args, const wchar_t *);
      s = _nei_log_wstr_to_mbs_or_placeholder(ws, ws_buf, sizeof(ws_buf));
    } else {
      s = _NEI_LOG_VA_ARG(args, const char *);
      if (s == NULL)
        s = "(null)";
    }
    len = strlen(s);
    if (len > _NEI_LOG_MAX_STRING_COPY)
      len = _NEI_LOG_MAX_STRING_COPY;
    len16 = (uint16_t)len;
    if (_nei_log_payload_write_u16(out, out_cap, used, len16) != 0)
      return -1;
    return _nei_log_payload_write_bytes(out, out_cap, used, s, len);
  }
  default:
    return -1;
  }
}

/* 1=ready plan, 0=fallback to scanner path, -1=hard reject (%n). */
static int _nei_log_build_fmt_plan(const char *fmt, _nei_log_fmt_plan_cache_st *plan) {
  const char *scan_ptr = fmt;
  if (fmt == NULL || plan == NULL) {
    return 0;
  }

  plan->op_count = 0U;
  plan->ready = 0U;
  plan->reject = 0U;

  while (*scan_ptr) {
    const char *p = scan_ptr;
    uint8_t payload_type = 0U;
    uint8_t conv_lm = 0U;
    if (*p != '%') {
      ++scan_ptr;
      continue;
    }
    ++p;
    if (*p == '%') {
      scan_ptr = p + 1;
      continue;
    }
    while (*p && _nei_log_is_flag_char(*p))
      ++p;
    if (*p == '*') {
      if (plan->op_count >= _NEI_LOG_FMT_PLAN_MAX_OPS) {
        return 0;
      }
      plan->ops[plan->op_count].payload_type = _NEI_LOG_PAYLOAD_I32;
      plan->ops[plan->op_count].conv_lm = 0U;
      ++plan->op_count;
      ++p;
    } else {
      while (*p && _nei_log_is_digit_char(*p))
        ++p;
    }
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        if (plan->op_count >= _NEI_LOG_FMT_PLAN_MAX_OPS) {
          return 0;
        }
        plan->ops[plan->op_count].payload_type = _NEI_LOG_PAYLOAD_I32;
        plan->ops[plan->op_count].conv_lm = 0U;
        ++plan->op_count;
        ++p;
      } else {
        while (*p && _nei_log_is_digit_char(*p))
          ++p;
      }
    }
    if (*p == 'h') {
      ++p;
      if (*p == 'h') {
        ++p;
        conv_lm = 1U;
      } else {
        conv_lm = 2U;
      }
    } else if (*p == 'l') {
      ++p;
      if (*p == 'l') {
        ++p;
        conv_lm = 4U;
      } else {
        conv_lm = 3U;
      }
    } else if (*p == 'j') {
      ++p;
      conv_lm = 5U;
    } else if (*p == 'z') {
      ++p;
      conv_lm = 6U;
    } else if (*p == 't') {
      ++p;
      conv_lm = 7U;
    } else if (*p == 'L') {
      ++p;
      conv_lm = 8U;
    }
    if (*p == '\0') {
      break;
    }
    if (*p == 'n') {
      plan->reject = 1U;
      return -1;
    }
    switch (*p) {
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
    case 'a':
    case 'A':
      payload_type = (conv_lm == 8U) ? _NEI_LOG_PAYLOAD_LONGDOUBLE : _NEI_LOG_PAYLOAD_DOUBLE;
      break;
    case 'd':
    case 'i':
      payload_type = (conv_lm <= 2U) ? _NEI_LOG_PAYLOAD_I32 : _NEI_LOG_PAYLOAD_I64;
      break;
    case 'u':
    case 'x':
    case 'X':
    case 'o':
      payload_type = (conv_lm <= 2U) ? _NEI_LOG_PAYLOAD_U32 : _NEI_LOG_PAYLOAD_U64;
      break;
    case 'c':
      payload_type = _NEI_LOG_PAYLOAD_CHAR;
      break;
    case 's':
      payload_type = _NEI_LOG_PAYLOAD_CSTR;
      break;
    case 'p':
      payload_type = _NEI_LOG_PAYLOAD_PTR;
      break;
    default:
      payload_type = 0U;
      break;
    }
    if (payload_type != 0U) {
      if (plan->op_count >= _NEI_LOG_FMT_PLAN_MAX_OPS) {
        return 0;
      }
      plan->ops[plan->op_count].payload_type = payload_type;
      plan->ops[plan->op_count].conv_lm = conv_lm;
      ++plan->op_count;
    }
    scan_ptr = p + 1;
  }

  plan->ready = 1U;
  return 1;
}

/** Windows: ACP (ANSI code page). POSIX: UTF-8 via iconv from WCHAR_T. */
static const char *_nei_log_wstr_to_mbs_or_placeholder(const wchar_t *ws, char *buf, size_t buf_cap) {
  static const char s_null_placeholder[] = "(null)";
  static const char s_encoding_error_placeholder[] = "[encoding error]";

  if (ws == NULL) {
    return s_null_placeholder;
  }
  if (buf == NULL || buf_cap == 0U) {
    return s_encoding_error_placeholder;
  }

  buf[0] = '\0';
  if (*ws == L'\0') {
    return buf;
  }

#if defined(_WIN32)
  size_t wlen = 0U;
  size_t lo;
  size_t hi;
  size_t best = 0U;
  int out_bytes = 0;

  while (ws[wlen] != L'\0') {
    ++wlen;
  }
  if (wlen > (size_t)INT_MAX) {
    return s_encoding_error_placeholder;
  }

  /* Use system ANSI code page (ACP), not UTF-8, so log text matches typical console/file encodings. */
  out_bytes = WideCharToMultiByte(CP_ACP, 0, ws, (int)wlen, NULL, 0, NULL, NULL);
  if (out_bytes > 0 && (size_t)out_bytes <= (buf_cap - 1U)) {
    int written = WideCharToMultiByte(CP_ACP, 0, ws, (int)wlen, buf, (int)(buf_cap - 1U), NULL, NULL);
    if (written <= 0) {
      return s_encoding_error_placeholder;
    }
    buf[written] = '\0';
    return buf;
  }

  lo = 0U;
  hi = wlen;
  while (lo <= hi) {
    size_t mid = lo + (hi - lo) / 2U;
    int need = 0;
    if (mid > 0U) {
      if (mid > (size_t)INT_MAX) {
        need = -1;
      } else {
        need = WideCharToMultiByte(CP_ACP, 0, ws, (int)mid, NULL, 0, NULL, NULL);
      }
    }
    if (need >= 0 && (size_t)need <= (buf_cap - 1U)) {
      best = mid;
      lo = mid + 1U;
    } else {
      if (mid == 0U) {
        break;
      }
      hi = mid - 1U;
    }
  }
  if (best == 0U) {
    return s_encoding_error_placeholder;
  }
  if (best > (size_t)INT_MAX) {
    return s_encoding_error_placeholder;
  }
  out_bytes = WideCharToMultiByte(CP_ACP, 0, ws, (int)best, buf, (int)(buf_cap - 1U), NULL, NULL);
  if (out_bytes <= 0) {
    return s_encoding_error_placeholder;
  }
  buf[out_bytes] = '\0';
  return buf;
#else
  size_t wlen = 0U;
  iconv_t cd;
  size_t in_left;
  size_t out_left;
  char *in_ptr;
  char *out_ptr;

  while (ws[wlen] != L'\0') {
    ++wlen;
  }
  if (wlen == 0U) {
    return buf;
  }
  if (wlen > (SIZE_MAX / sizeof(wchar_t))) {
    return s_encoding_error_placeholder;
  }

  cd = iconv_open("UTF-8", "WCHAR_T");
  if (cd == (iconv_t)-1) {
    return s_encoding_error_placeholder;
  }

  in_left = wlen * sizeof(wchar_t);
  out_left = buf_cap - 1U;
  in_ptr = (char *)(void *)ws;
  out_ptr = buf;

  while (in_left > 0U) {
    size_t rc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
    if (rc != (size_t)-1) {
      break;
    }
    if (errno == E2BIG) {
      break;
    }
    (void)iconv_close(cd);
    return s_encoding_error_placeholder;
  }

  (void)iconv_close(cd);
  *out_ptr = '\0';
  return buf;
#endif
}

#pragma endregion

#pragma region serialization functions

size_t _nei_log_serialize_event(uint8_t *out,
                                size_t out_cap,
                                nei_log_config_handle_t config_handle,
                                const char *file,
                                int32_t line,
                                const char *func,
                                int32_t level,
                                int32_t verbose,
                                const char *fmt,
                                va_list args) {
  size_t used;
  size_t aligned_size;
  const char *scan_ptr;
  const _nei_log_fmt_plan_cache_st *plan = NULL;
  uint16_t op_idx;
  nei_log_event_header_st header;

  if (out == NULL || fmt == NULL || out_cap < sizeof(nei_log_event_header_st)) {
    return 0;
  }

  memset(&header, 0, sizeof(header));
  header.timestamp_ns = _nei_log_now_ns();
  header.config_handle = config_handle;
  header.file_ptr = file;
  header.func_ptr = func;
  header.fmt_ptr = fmt;
  header.level = level;
  header.line = line;
  header.verbose = verbose;
  _nei_log_header_fill_thread_id(&header, config_handle);

  /* Reserve header space; write header once after total_size is known. */
  used = sizeof(header);

  if (s_tls_fmt_plan_cache.fmt != fmt) {
    int plan_rc;
    s_tls_fmt_plan_cache.fmt = fmt;
    plan_rc = _nei_log_build_fmt_plan(fmt, &s_tls_fmt_plan_cache);
    if (plan_rc < 0) {
      return 0;
    }
  }

  if (s_tls_fmt_plan_cache.fmt == fmt && s_tls_fmt_plan_cache.ready != 0U && s_tls_fmt_plan_cache.reject == 0U) {
    plan = &s_tls_fmt_plan_cache;
  }

  if (plan != NULL) {
    for (op_idx = 0U; op_idx < plan->op_count; ++op_idx) {
      if (_nei_log_payload_emit_arg(out,
                                    out_cap,
                                    &used,
                                    plan->ops[op_idx].payload_type,
                                    plan->ops[op_idx].conv_lm,
                                    _NEI_LOG_VA_PASS(args)) != 0) {
        return 0;
      }
    }

    aligned_size = _nei_log_align_up_8(used);
    if (_nei_log_payload_write_padded_zero(out, out_cap, &used, aligned_size) != 0) {
      return 0;
    }

    header.total_size = (uint32_t)used;
    memcpy(out, &header, sizeof(header));
    return used;
  }

  scan_ptr = fmt;

  while (*scan_ptr) {
    const char *p = scan_ptr;
    uint8_t payload_type = 0;
    int conv_lm = 0;
    if (*p != '%') {
      ++scan_ptr;
      continue;
    }
    ++p;
    if (*p == '%') {
      scan_ptr = p + 1;
      continue;
    }
    while (*p && _nei_log_is_flag_char(*p))
      ++p;
    if (*p == '*') {
      int32_t v = va_arg(args, int);
      if (_nei_log_payload_write_u8(out, out_cap, &used, _NEI_LOG_PAYLOAD_I32) != 0)
        return 0;
      if (_nei_log_payload_write_bytes(out, out_cap, &used, &v, sizeof(v)) != 0)
        return 0;
      ++p;
    } else {
      while (*p && _nei_log_is_digit_char(*p))
        ++p;
    }
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        int32_t v = va_arg(args, int);
        if (_nei_log_payload_write_u8(out, out_cap, &used, _NEI_LOG_PAYLOAD_I32) != 0)
          return 0;
        if (_nei_log_payload_write_bytes(out, out_cap, &used, &v, sizeof(v)) != 0)
          return 0;
        ++p;
      } else {
        while (*p && _nei_log_is_digit_char(*p))
          ++p;
      }
    }
    if (*p == 'h') {
      ++p;
      if (*p == 'h') {
        ++p;
        conv_lm = 1;
      } else {
        conv_lm = 2;
      }
    } else if (*p == 'l') {
      ++p;
      if (*p == 'l') {
        ++p;
        conv_lm = 4;
      } else {
        conv_lm = 3;
      }
    } else if (*p == 'j') {
      ++p;
      conv_lm = 5;
    } else if (*p == 'z') {
      ++p;
      conv_lm = 6;
    } else if (*p == 't') {
      ++p;
      conv_lm = 7;
    } else if (*p == 'L') {
      ++p;
      conv_lm = 8;
    }
    if (*p == '\0') {
      break;
    }
    /* Reject %n: writing through a captured pointer is unsafe for async/binary logs. */
    if (*p == 'n') {
      return 0;
    }
    switch (*p) {
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
    case 'a':
    case 'A':
      payload_type = (conv_lm == 8) ? _NEI_LOG_PAYLOAD_LONGDOUBLE : _NEI_LOG_PAYLOAD_DOUBLE;
      break;
    case 'd':
    case 'i':
      if (conv_lm == 0 || conv_lm == 1 || conv_lm == 2) {
        payload_type = _NEI_LOG_PAYLOAD_I32;
      } else {
        payload_type = _NEI_LOG_PAYLOAD_I64;
      }
      break;
    case 'u':
    case 'x':
    case 'X':
    case 'o':
      if (conv_lm == 0 || conv_lm == 1 || conv_lm == 2) {
        payload_type = _NEI_LOG_PAYLOAD_U32;
      } else {
        payload_type = _NEI_LOG_PAYLOAD_U64;
      }
      break;
    case 'c':
      payload_type = _NEI_LOG_PAYLOAD_CHAR;
      break;
    case 's':
      payload_type = _NEI_LOG_PAYLOAD_CSTR;
      break;
    case 'p':
      payload_type = _NEI_LOG_PAYLOAD_PTR;
      break;
    default:
      payload_type = 0;
      break;
    }
    if (payload_type != 0) {
      if (_nei_log_payload_emit_arg(out, out_cap, &used, payload_type, (uint8_t)conv_lm, _NEI_LOG_VA_PASS(args)) != 0) {
        return 0;
      }
    }
    scan_ptr = (*p) ? (p + 1) : p;
  }

  aligned_size = _nei_log_align_up_8(used);
  if (_nei_log_payload_write_padded_zero(out, out_cap, &used, aligned_size) != 0) {
    return 0;
  }

  header.total_size = (uint32_t)used;
  memcpy(out, &header, sizeof(header));
  return used;
}

size_t _nei_log_serialize_literal_msg(uint8_t *out,
                                      size_t out_cap,
                                      nei_log_config_handle_t config_handle,
                                      const char *file,
                                      int32_t line,
                                      const char *func,
                                      int32_t level,
                                      int32_t verbose,
                                      const char *message,
                                      size_t message_length) {
  nei_log_event_header_st header;
  size_t used;
  size_t aligned_size;
  size_t copy_len;
  uint16_t len16;

  if (out == NULL || out_cap < sizeof(nei_log_event_header_st)) {
    return 0;
  }

  copy_len = message_length;
  if (message == NULL) {
    copy_len = 0U;
  } else if (copy_len > _NEI_LOG_MAX_STRING_COPY) {
    copy_len = _NEI_LOG_MAX_STRING_COPY;
  }
  len16 = (uint16_t)copy_len;

  memset(&header, 0, sizeof(header));
  header.timestamp_ns = _nei_log_now_ns();
  header.config_handle = config_handle;
  header.file_ptr = file;
  header.func_ptr = func;
  header.fmt_ptr = NULL;
  header.level = level;
  header.line = line;
  header.verbose = verbose;
  _nei_log_header_fill_thread_id(&header, config_handle);

  /* Reserve header space; write header once after total_size is known. */
  used = sizeof(header);

  if (_nei_log_payload_write_u8(out, out_cap, &used, _NEI_LOG_PAYLOAD_LITERAL_MSG) != 0) {
    return 0;
  }
  if (_nei_log_payload_write_u16(out, out_cap, &used, len16) != 0) {
    return 0;
  }
  if (copy_len > 0U && message != NULL) {
    if (_nei_log_payload_write_bytes(out, out_cap, &used, message, copy_len) != 0) {
      return 0;
    }
  }

  aligned_size = _nei_log_align_up_8(used);
  if (_nei_log_payload_write_padded_zero(out, out_cap, &used, aligned_size) != 0) {
    return 0;
  }

  header.total_size = (uint32_t)used;
  memcpy(out, &header, sizeof(header));
  return used;
}

#pragma endregion

#pragma region time support

#if defined(_WIN32)
typedef VOID(WINAPI *_nei_pfn_GetSystemTimePreciseAsFileTime)(LPFILETIME);

static INIT_ONCE s_nei_log_win_time_once = INIT_ONCE_STATIC_INIT;
static _nei_pfn_GetSystemTimePreciseAsFileTime s_nei_log_pfn_GetSystemTimePreciseAsFileTime;

static BOOL CALLBACK _nei_log_win_time_init_once(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
  HMODULE k32;
  (void)init_once;
  (void)parameter;
  (void)context;
  k32 = GetModuleHandleW(L"kernel32.dll");
  if (k32 != NULL) {
    s_nei_log_pfn_GetSystemTimePreciseAsFileTime =
        (_nei_pfn_GetSystemTimePreciseAsFileTime)(void *)GetProcAddress(k32, "GetSystemTimePreciseAsFileTime");
  }
  return TRUE;
}

static void _nei_log_win32_get_system_time_as_filetime(LPFILETIME ft) {
  (void)InitOnceExecuteOnce(&s_nei_log_win_time_once, _nei_log_win_time_init_once, NULL, NULL);
  if (s_nei_log_pfn_GetSystemTimePreciseAsFileTime != NULL) {
    s_nei_log_pfn_GetSystemTimePreciseAsFileTime(ft);
  } else {
    GetSystemTimeAsFileTime(ft);
  }
}
#endif

uint64_t _nei_log_now_ns(void) {
#if defined(_WIN32)
  FILETIME ft;
  ULARGE_INTEGER uli;
  const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
  _nei_log_win32_get_system_time_as_filetime(&ft);
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  if (uli.QuadPart <= EPOCH_DIFF_100NS) {
    return 0;
  }
  return (uli.QuadPart - EPOCH_DIFF_100NS) * 100ULL;
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 0;
  }
  return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
#endif
}

#pragma endregion
