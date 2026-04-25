#include "log_internal.h"

#pragma region level string tables

// clang-format off
#define _NEI_LOG_LVL_TAGS \
  _NEI_LOG_LVL_STR_PAIRS("VERBOSE", "V"), \
  _NEI_LOG_LVL_STR_PAIRS("TRACE",   "T"), \
  _NEI_LOG_LVL_STR_PAIRS("DEBUG",   "D"), \
  _NEI_LOG_LVL_STR_PAIRS("INFO",    "I"), \
  _NEI_LOG_LVL_STR_PAIRS("WARN",    "W"), \
  _NEI_LOG_LVL_STR_PAIRS("ERROR",   "E"), \
  _NEI_LOG_LVL_STR_PAIRS("FATAL",   "F")
// clang-format on
#define _NEI_LOG_LVL_STR_PAIRS(_longstr, _shortstr) _longstr
/** @brief Full log level tag table. */
const char *s_level_strings[] = {_NEI_LOG_LVL_TAGS};
#undef _NEI_LOG_LVL_STR_PAIRS
#define _NEI_LOG_LVL_STR_PAIRS(_longstr, _shortstr) _shortstr
/** @brief Short log level tag table. */
const char *s_level_short_strings[] = {_NEI_LOG_LVL_TAGS};
#undef _NEI_LOG_LVL_STR_PAIRS
#undef _NEI_LOG_LVL_TAGS

/** @brief Get the full log level tag. */
const char *_get_level_string(nei_log_level_e level) {
  assert(level >= NEI_L_VERBOSE && level <= NEI_L_FATAL);
  return s_level_strings[level];
}

/** @brief Get the short log level tag. */
const char *_get_level_short_string(nei_log_level_e level) {
  assert(level >= NEI_L_VERBOSE && level <= NEI_L_FATAL);
  return s_level_short_strings[level];
}

#pragma endregion

#pragma region timestamp formatting

typedef struct _nei_log_ts_cache_st {
  int ready;
  time_t sec;
  nei_log_timestamp_style_e style;
  char datetime[48];
  size_t datetime_len;
  char tz[16];
  int has_tz;
} nei_log_ts_cache_st;

static _NEI_LOG_TLS nei_log_ts_cache_st s_tls_ts_cache;

static int _nei_log_format_tz_offset(time_t sec, char *out, size_t out_size) {
  struct tm gm_tm;
  time_t gm_as_local;
  long offset_sec;
  long abs_sec;
  long hh;
  long mm;

  if (out == NULL || out_size < 7U) {
    return -1;
  }
#if defined(_WIN32)
  gmtime_s(&gm_tm, &sec);
#else
  gmtime_r(&sec, &gm_tm);
#endif
  gm_as_local = mktime(&gm_tm);
  if (gm_as_local == (time_t)-1) {
    return -1;
  }
  offset_sec = (long)difftime(sec, gm_as_local);
  abs_sec = labs(offset_sec);
  hh = abs_sec / 3600L;
  mm = (abs_sec % 3600L) / 60L;
  (void)snprintf(out, out_size, "%c%02ld:%02ld", (offset_sec >= 0L) ? '+' : '-', hh, mm);
  return 0;
}

void _nei_log_format_timestamp(uint64_t timestamp_ns, nei_log_timestamp_style_e style, char *out, size_t out_size) {
  time_t sec;
  unsigned millis;
  unsigned nanos;
  struct tm tm_buf;
  size_t n = 0U;
  int cache_hit;
  nei_log_ts_cache_st *cache = &s_tls_ts_cache;

  if (out == NULL || out_size == 0U) {
    return;
  }
  out[0] = '\0';

  if (style == NEI_LOG_TIMESTAMP_STYLE_NONE) {
    return;
  }

  sec = (time_t)(timestamp_ns / 1000000000ULL);
  millis = (unsigned)((timestamp_ns % 1000000000ULL) / 1000000ULL);
  nanos = (unsigned)(timestamp_ns % 1000000000ULL);

  cache_hit = cache->ready && cache->sec == sec && cache->style == style;
  if (!cache_hit) {
#if defined(_WIN32)
    localtime_s(&tm_buf, &sec);
#else
    localtime_r(&sec, &tm_buf);
#endif
    cache->has_tz = 0;
    cache->tz[0] = '\0';
    cache->datetime_len = 0U;

    switch (style) {
    case NEI_LOG_TIMESTAMP_STYLE_ISO8601_MS:
      n = strftime(cache->datetime, sizeof(cache->datetime), "%Y-%m-%dT%H:%M:%S", &tm_buf);
      break;
    case NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS:
    case NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS_NSEC:
      n = strftime(cache->datetime, sizeof(cache->datetime), "%Y-%m-%dT%H:%M:%S", &tm_buf);
      if (n > 0U) {
        if (_nei_log_format_tz_offset(sec, cache->tz, sizeof(cache->tz)) != 0) {
          (void)snprintf(cache->tz, sizeof(cache->tz), "+00:00");
        }
        cache->has_tz = 1;
      }
      break;
    case NEI_LOG_TIMESTAMP_STYLE_DEFAULT:
    default:
      n = strftime(cache->datetime, sizeof(cache->datetime), "%Y-%m-%d %H:%M:%S", &tm_buf);
      break;
    }
    if (n > 0U) {
      cache->datetime_len = n;
      cache->sec = sec;
      cache->style = style;
      cache->ready = 1;
    }
  }

  if (cache->ready && cache->sec == sec && cache->style == style) {
    if (cache->has_tz) {
      if (style == NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS_NSEC) {
        (void)snprintf(out, out_size, "%s.%09u%s", cache->datetime, nanos, cache->tz);
      } else {
        (void)snprintf(out, out_size, "%s.%03u%s", cache->datetime, millis, cache->tz);
      }
    } else {
      (void)snprintf(out, out_size, "%s.%03u", cache->datetime, millis);
    }
    return;
  }

  /* Conservative fallback when cache fill/formatting fails. */
#if defined(_WIN32)
  localtime_s(&tm_buf, &sec);
#else
  localtime_r(&sec, &tm_buf);
#endif
  if (style == NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS || style == NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS_NSEC) {
    char tzbuf[16];
    if (_nei_log_format_tz_offset(sec, tzbuf, sizeof(tzbuf)) != 0) {
      (void)snprintf(tzbuf, sizeof(tzbuf), "+00:00");
    }
    if (style == NEI_LOG_TIMESTAMP_STYLE_RFC3339_FULL_MS_NSEC) {
      (void)snprintf(out,
                     out_size,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%09u%s",
                     tm_buf.tm_year + 1900,
                     tm_buf.tm_mon + 1,
                     tm_buf.tm_mday,
                     tm_buf.tm_hour,
                     tm_buf.tm_min,
                     tm_buf.tm_sec,
                     nanos,
                     tzbuf);
    } else {
      (void)snprintf(out,
                     out_size,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%03u%s",
                     tm_buf.tm_year + 1900,
                     tm_buf.tm_mon + 1,
                     tm_buf.tm_mday,
                     tm_buf.tm_hour,
                     tm_buf.tm_min,
                     tm_buf.tm_sec,
                     millis,
                     tzbuf);
    }
    return;
  }
  if (style == NEI_LOG_TIMESTAMP_STYLE_ISO8601_MS) {
    (void)snprintf(out,
                   out_size,
                   "%04d-%02d-%02dT%02d:%02d:%02d.%03u",
                   tm_buf.tm_year + 1900,
                   tm_buf.tm_mon + 1,
                   tm_buf.tm_mday,
                   tm_buf.tm_hour,
                   tm_buf.tm_min,
                   tm_buf.tm_sec,
                   millis);
  } else {
    (void)snprintf(out,
                   out_size,
                   "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                   tm_buf.tm_year + 1900,
                   tm_buf.tm_mon + 1,
                   tm_buf.tm_mday,
                   tm_buf.tm_hour,
                   tm_buf.tm_min,
                   tm_buf.tm_sec,
                   millis);
  }
}

#pragma endregion

#pragma region event formatting

typedef struct _nei_log_fmt_cache_st {
  const char *fmt;
  size_t fmt_len;
  int has_percent;
  int simple_replay;
} nei_log_fmt_cache_st;

static _NEI_LOG_TLS nei_log_fmt_cache_st s_tls_fmt_cache;

static void _nei_log_snprintf_i64(const char *spec, char *tmp, size_t tcap, int64_t v);
static void _nei_log_snprintf_u64(const char *spec, char *tmp, size_t tcap, uint64_t v);
static int _nei_log_read_u16(const uint8_t **cursor, const uint8_t *end, uint16_t *out_value);
static int _nei_log_read_bytes(const uint8_t **cursor, const uint8_t *end, void *dst, size_t n);

static int _nei_log_is_simple_replay_fmt(const char *fmt) {
  const char *p = fmt;
  if (fmt == NULL) {
    return 0;
  }
  while (*p) {
    if (*p != '%') {
      ++p;
      continue;
    }
    ++p;
    if (*p == '%') {
      ++p;
      continue;
    }
    if (*p == '\0') {
      return 0;
    }
    if (*p == 'd' || *p == 'i' || *p == 'u' || *p == 's') {
      ++p;
      continue;
    }
    return 0;
  }
  return 1;
}

static void _nei_log_get_fmt_meta_cached(const char *fmt, int *out_has_percent, size_t *out_len, int *out_simple_replay) {
  if (out_has_percent == NULL || out_len == NULL || out_simple_replay == NULL) {
    return;
  }
  if (fmt == NULL) {
    *out_has_percent = 0;
    *out_len = 0U;
    *out_simple_replay = 0;
    return;
  }
  if (s_tls_fmt_cache.fmt == fmt) {
    *out_has_percent = s_tls_fmt_cache.has_percent;
    *out_len = s_tls_fmt_cache.fmt_len;
    *out_simple_replay = s_tls_fmt_cache.simple_replay;
    return;
  }
  s_tls_fmt_cache.fmt = fmt;
  s_tls_fmt_cache.fmt_len = strlen(fmt);
  s_tls_fmt_cache.has_percent = (strchr(fmt, '%') != NULL) ? 1 : 0;
  s_tls_fmt_cache.simple_replay = _nei_log_is_simple_replay_fmt(fmt);
  *out_has_percent = s_tls_fmt_cache.has_percent;
  *out_len = s_tls_fmt_cache.fmt_len;
  *out_simple_replay = s_tls_fmt_cache.simple_replay;
}

static int _nei_log_append_char(char *out, size_t cap, size_t *used, char c) {
  if (out == NULL || used == NULL || *used + 1U >= cap) {
    return -1;
  }
  out[*used] = c;
  *used += 1U;
  out[*used] = '\0';
  return 0;
}

static int _nei_log_append_nstr(char *out, size_t cap, size_t *used, const char *s, size_t n) {
  if (out == NULL || used == NULL || s == NULL || *used + n >= cap) {
    return -1;
  }
  memcpy(out + *used, s, n);
  *used += n;
  out[*used] = '\0';
  return 0;
}

static int _nei_log_append_cstr(char *out, size_t cap, size_t *used, const char *s) {
  return _nei_log_append_nstr(out, cap, used, s, strlen(s));
}

static int _nei_log_append_u64_decimal(char *out, size_t cap, size_t *used, uint64_t v) {
  char buf[32];
  char *p = buf + sizeof(buf);
  do {
    --p;
    *p = (char)('0' + (char)(v % 10U));
    v /= 10U;
  } while (v != 0U);
  return _nei_log_append_nstr(out, cap, used, p, (size_t)((buf + sizeof(buf)) - p));
}

static int _nei_log_append_i64_decimal(char *out, size_t cap, size_t *used, int64_t v) {
  uint64_t mag;
  if (v < 0) {
    if (_nei_log_append_char(out, cap, used, '-') != 0) {
      return -1;
    }
    mag = (uint64_t)(-(v + 1)) + 1U;
  } else {
    mag = (uint64_t)v;
  }
  return _nei_log_append_u64_decimal(out, cap, used, mag);
}

static int _nei_log_is_simple_spec(const char *spec) {
  if (spec == NULL || spec[0] != '%') {
    return 0;
  }
  if (spec[1] == '%' && spec[2] == '\0') {
    return 0;
  }
  const char *p = spec + 1;
  if (!_nei_log_is_flag_char(*p) && !_nei_log_is_digit_char(*p) && *p != '-' && *p != '+' && *p != ' ' &&
      *p != '#' && *p != '0' && *p != '.' && *p != '*') {
    if (*p == 'l') {
      ++p;
      if (*p == 'l') {
        ++p;
      }
    } else if (*p == 'h') {
      ++p;
      if (*p == 'h') {
        ++p;
      }
    }
    if ((*p == 'd' || *p == 'i' || *p == 'u' || *p == 's' || *p == 'x' || *p == 'X' || *p == 'o') && p[1] == '\0') {
      return 1;
    }
  }
  return 0;
}

static int _nei_log_append_from_spec(const char *spec,
                                      uint8_t payload_type,
                                      const uint8_t **cursor,
                                      const uint8_t *end,
                                      char *out,
                                      size_t out_cap,
                                      size_t *used) {
  if (spec == NULL || spec[1] == '\0') {
    return -1;
  }
  const char *p = spec + 1;
  if (*p == 'l') {
    ++p;
    if (*p == 'l') {
      ++p;
    }
  } else if (*p == 'h') {
    ++p;
    if (*p == 'h') {
      ++p;
    }
  }
  char conv_char = *p;
  switch (conv_char) {
  case 'd':
  case 'i': {
    int64_t v = 0;
    if (payload_type == _NEI_LOG_PAYLOAD_I32) {
      int32_t v32 = 0;
      if (_nei_log_read_bytes(cursor, end, &v32, sizeof(v32)) != 0) {
        return -1;
      }
      v = (int64_t)v32;
    } else if (payload_type == _NEI_LOG_PAYLOAD_I64) {
      if (_nei_log_read_bytes(cursor, end, &v, sizeof(v)) != 0) {
        return -1;
      }
    } else {
      return -1;
    }
    return _nei_log_append_i64_decimal(out, out_cap, used, v);
  }
  case 'u':
  case 'x':
  case 'X':
  case 'o': {
    uint64_t v = 0U;
    if (payload_type == _NEI_LOG_PAYLOAD_U32) {
      uint32_t v32 = 0U;
      if (_nei_log_read_bytes(cursor, end, &v32, sizeof(v32)) != 0) {
        return -1;
      }
      v = (uint64_t)v32;
    } else if (payload_type == _NEI_LOG_PAYLOAD_U64) {
      if (_nei_log_read_bytes(cursor, end, &v, sizeof(v)) != 0) {
        return -1;
      }
    } else {
      return -1;
    }
    if (conv_char == 'u') {
      return _nei_log_append_u64_decimal(out, out_cap, used, v);
    } else {
      char buf[32];
      snprintf(buf, sizeof(buf), (conv_char == 'x') ? "%llx" : (conv_char == 'X') ? "%llX" : "%llo",
               (unsigned long long)v);
      return _nei_log_append_cstr(out, out_cap, used, buf);
    }
  }
  case 's': {
    uint16_t len16 = 0;
    if (payload_type != _NEI_LOG_PAYLOAD_CSTR) {
      return -1;
    }
    if (_nei_log_read_u16(cursor, end, &len16) != 0) {
      return -1;
    }
    if (len16 > _NEI_LOG_MAX_STRING_COPY || *cursor + len16 > end) {
      return -1;
    }
    if (len16 > 0U && _nei_log_append_nstr(out, out_cap, used, (const char *)(*cursor), (size_t)len16) != 0) {
      return -1;
    }
    *cursor += len16;
    return 0;
  }
  default:
    return -1;
  }
}

static int _nei_log_read_u16(const uint8_t **cursor, const uint8_t *end, uint16_t *out_value) {
  if (cursor == NULL || *cursor == NULL || end == NULL || out_value == NULL || *cursor + sizeof(uint16_t) > end) {
    return -1;
  }
  memcpy(out_value, *cursor, sizeof(uint16_t));
  *cursor += sizeof(uint16_t);
  return 0;
}

static int _nei_log_read_bytes(const uint8_t **cursor, const uint8_t *end, void *dst, size_t n) {
  if (cursor == NULL || *cursor == NULL || end == NULL || dst == NULL || *cursor + n > end) {
    return -1;
  }
  memcpy(dst, *cursor, n);
  *cursor += n;
  return 0;
}

static void _nei_log_snprintf_i64(const char *spec, char *tmp, size_t tcap, int64_t v) {
  if (strstr(spec, "ll") != NULL) {
    snprintf(tmp, tcap, spec, (long long)v);
  } else if (strchr(spec, 'j')) {
    snprintf(tmp, tcap, spec, (intmax_t)v);
  } else if (strchr(spec, 'z') && strchr(spec, 'u')) {
    snprintf(tmp, tcap, spec, (size_t)(uint64_t)v);
  } else if (strchr(spec, 'z')) {
    snprintf(tmp, tcap, spec, (ptrdiff_t)v);
  } else if (strchr(spec, 't')) {
    if (strchr(spec, 'u') || strchr(spec, 'x') || strchr(spec, 'X') || strchr(spec, 'o')) {
      snprintf(tmp, tcap, spec, (size_t)(uint64_t)v);
    } else {
      snprintf(tmp, tcap, spec, (ptrdiff_t)v);
    }
  } else if (strchr(spec, 'l')) {
    snprintf(tmp, tcap, spec, (long)v);
  } else {
    snprintf(tmp, tcap, spec, (int)v);
  }
}

static void _nei_log_snprintf_u64(const char *spec, char *tmp, size_t tcap, uint64_t v) {
  if (strstr(spec, "ll") != NULL) {
    snprintf(tmp, tcap, spec, (unsigned long long)v);
  } else if (strchr(spec, 'j')) {
    snprintf(tmp, tcap, spec, (uintmax_t)v);
  } else if (strchr(spec, 'z')) {
    snprintf(tmp, tcap, spec, (size_t)v);
  } else if (strchr(spec, 't')) {
    snprintf(tmp, tcap, spec, (size_t)v);
  } else if (strchr(spec, 'l')) {
    snprintf(tmp, tcap, spec, (unsigned long)v);
  } else {
    snprintf(tmp, tcap, spec, (unsigned int)v);
  }
}

static int _nei_log_build_runtime_conversion_spec(const char *scan,
                                                  const char **after,
                                                  const uint8_t **cursor,
                                                  const uint8_t *end,
                                                  uint8_t *parsed_args,
                                                  char *spec,
                                                  size_t spec_cap) {
  const char *p;
  size_t n = 0U;
  if (scan == NULL || after == NULL || cursor == NULL || *cursor == NULL || end == NULL || parsed_args == NULL
      || spec == NULL || spec_cap < 3U || *scan != '%') {
    return -1;
  }

  p = scan;
  spec[n++] = *p++;
  if (*p == '%') {
    if (n + 2U >= spec_cap) {
      return -1;
    }
    spec[n++] = *p++;
    spec[n] = '\0';
    *after = p;
    return 0;
  }

  while (*p && _nei_log_is_flag_char(*p)) {
    if (n + 2U >= spec_cap)
      return -1;
    spec[n++] = *p++;
  }
  if (*p == '*') {
    int32_t width = 0;
    char tmp[16];
    uint8_t payload_type;
    if (*cursor >= end || *parsed_args == UINT8_MAX)
      return -1;
    payload_type = **cursor;
    (*cursor)++;
    if (payload_type != _NEI_LOG_PAYLOAD_I32)
      return -1;
    if (_nei_log_read_bytes(cursor, end, &width, sizeof(width)) != 0)
      return -1;
    (*parsed_args)++;
    snprintf(tmp, sizeof(tmp), "%d", (int)width);
    if (n + strlen(tmp) + 2U >= spec_cap)
      return -1;
    memcpy(spec + n, tmp, strlen(tmp));
    n += strlen(tmp);
    ++p;
  } else {
    while (*p && _nei_log_is_digit_char(*p)) {
      if (n + 2U >= spec_cap)
        return -1;
      spec[n++] = *p++;
    }
  }
  if (*p == '.') {
    if (n + 2U >= spec_cap)
      return -1;
    spec[n++] = *p++;
    if (*p == '*') {
      int32_t precision = 0;
      char tmp[16];
      uint8_t payload_type;
      if (*cursor >= end || *parsed_args == UINT8_MAX)
        return -1;
      payload_type = **cursor;
      (*cursor)++;
      if (payload_type != _NEI_LOG_PAYLOAD_I32)
        return -1;
      if (_nei_log_read_bytes(cursor, end, &precision, sizeof(precision)) != 0)
        return -1;
      (*parsed_args)++;
      if (precision < 0) {
        precision = 0;
      }
      snprintf(tmp, sizeof(tmp), "%d", (int)precision);
      if (n + strlen(tmp) + 2U >= spec_cap)
        return -1;
      memcpy(spec + n, tmp, strlen(tmp));
      n += strlen(tmp);
      ++p;
    } else {
      while (*p && _nei_log_is_digit_char(*p)) {
        if (n + 2U >= spec_cap)
          return -1;
        spec[n++] = *p++;
      }
    }
  }
  if (*p == 'h' || *p == 'l') {
    char first = *p;
    ++p;
    if (*p == first) {
      if (n + 3U >= spec_cap)
        return -1;
      spec[n++] = first;
      spec[n++] = *p++;
    } else if (first == 'l' && *p == 's') {
      /* Runtime formatting always replays narrow bytes, so keep %ls as %s in replay spec. */
    } else {
      if (n + 2U >= spec_cap)
        return -1;
      spec[n++] = first;
    }
  } else if (*p == 'j' || *p == 'z' || *p == 't') {
    if (n + 2U >= spec_cap)
      return -1;
    spec[n++] = *p++;
  } else if (*p == 'L') {
    if (n + 2U >= spec_cap)
      return -1;
    spec[n++] = *p++;
  }
  if (*p == '\0') {
    return -1;
  }
  if (n + 2U >= spec_cap) {
    return -1;
  }
  spec[n++] = *p++;
  spec[n] = '\0';
  *after = p;
  return 0;
}

static int _nei_log_format_simple_replay(const char *fmt,
                                         const uint8_t **cursor,
                                         const uint8_t *end,
                                         char *out,
                                         size_t out_cap,
                                         size_t *used) {
  const char *scan = fmt;
  if (fmt == NULL || cursor == NULL || *cursor == NULL || end == NULL || out == NULL || used == NULL) {
    return -1;
  }

  while (*scan) {
    if (*scan != '%') {
      if (_nei_log_append_char(out, out_cap, used, *scan) != 0) {
        return -1;
      }
      ++scan;
      continue;
    }

    ++scan;
    if (*scan == '%') {
      if (_nei_log_append_char(out, out_cap, used, '%') != 0) {
        return -1;
      }
      ++scan;
      continue;
    }
    if (*scan == '\0' || *cursor >= end) {
      return -1;
    }

    {
      uint8_t payload_type = *(*cursor)++;
      switch (*scan) {
      case 'd':
      case 'i': {
        int64_t v = 0;
        if (payload_type == _NEI_LOG_PAYLOAD_I32) {
          int32_t v32 = 0;
          if (_nei_log_read_bytes(cursor, end, &v32, sizeof(v32)) != 0) {
            return -1;
          }
          v = (int64_t)v32;
        } else if (payload_type == _NEI_LOG_PAYLOAD_I64) {
          if (_nei_log_read_bytes(cursor, end, &v, sizeof(v)) != 0) {
            return -1;
          }
        } else {
          return -1;
        }
        if (_nei_log_append_i64_decimal(out, out_cap, used, v) != 0) {
          return -1;
        }
        break;
      }
      case 'u': {
        uint64_t v = 0U;
        if (payload_type == _NEI_LOG_PAYLOAD_U32) {
          uint32_t v32 = 0U;
          if (_nei_log_read_bytes(cursor, end, &v32, sizeof(v32)) != 0) {
            return -1;
          }
          v = (uint64_t)v32;
        } else if (payload_type == _NEI_LOG_PAYLOAD_U64) {
          if (_nei_log_read_bytes(cursor, end, &v, sizeof(v)) != 0) {
            return -1;
          }
        } else {
          return -1;
        }
        if (_nei_log_append_u64_decimal(out, out_cap, used, v) != 0) {
          return -1;
        }
        break;
      }
      case 's': {
        uint16_t len16 = 0;
        if (payload_type != _NEI_LOG_PAYLOAD_CSTR) {
          return -1;
        }
        if (_nei_log_read_u16(cursor, end, &len16) != 0) {
          return -1;
        }
        if (len16 > _NEI_LOG_MAX_STRING_COPY || *cursor + len16 > end) {
          return -1;
        }
        if (len16 > 0U && _nei_log_append_nstr(out, out_cap, used, (const char *)(*cursor), (size_t)len16) != 0) {
          return -1;
        }
        *cursor += len16;
        break;
      }
      default:
        return -1;
      }
    }
    ++scan;
  }

  return 0;
}

static const char *_nei_log_basename(const char *path) {
  const char *slash;
  const char *backslash;
  if (path == NULL) {
    return NULL;
  }
  slash = strrchr(path, '/');
  backslash = strrchr(path, '\\');
  if (slash == NULL && backslash == NULL) {
    return path;
  }
  if (slash == NULL) {
    return backslash + 1;
  }
  if (backslash == NULL) {
    return slash + 1;
  }
  return (slash > backslash) ? (slash + 1) : (backslash + 1);
}

static int _nei_log_append_location_block(
    const nei_log_event_header_st *header, int short_path, char *out, size_t out_cap, size_t *used) {
  if (header == NULL || out == NULL || used == NULL) {
    return -1;
  }

  if (header->file_ptr != NULL) {
    const char *path = short_path ? _nei_log_basename(header->file_ptr) : header->file_ptr;
    if (_nei_log_append_cstr(out, out_cap, used, path) != 0)
      return -1;
    if (_nei_log_append_char(out, out_cap, used, ':') != 0)
      return -1;
    {
      char line_buf[16];
      snprintf(line_buf, sizeof(line_buf), "%d", (int)header->line);
      if (_nei_log_append_cstr(out, out_cap, used, line_buf) != 0)
        return -1;
    }
  }

  if (header->func_ptr != NULL) {
    if (header->file_ptr != NULL) {
      if (_nei_log_append_char(out, out_cap, used, ' ') != 0)
        return -1;
    }
    if (_nei_log_append_cstr(out, out_cap, used, header->func_ptr) != 0)
      return -1;
  }

  return 0;
}

int _nei_log_format_event(const nei_log_event_header_st *header,
                          const nei_log_config_st *effective_config,
                          const uint8_t *payload,
                          size_t payload_size,
                          char *out,
                          size_t out_cap) {
  const char *fmt;
  const char *scan;
  const uint8_t *cursor = payload;
  const uint8_t *end = payload + payload_size;
  size_t used = 0U;
  uint8_t parsed_args = 0U;
  int short_level_tag;
  int short_path;
  int log_location;
  int log_location_after_message;
  nei_log_timestamp_style_e ts_style;

  if (header == NULL || effective_config == NULL || payload == NULL || out == NULL || out_cap == 0U) {
    return -1;
  }
  short_level_tag = effective_config->short_level_tag;
  short_path = effective_config->short_path;
  log_location = effective_config->log_location;
  log_location_after_message = effective_config->log_location_after_message;
  ts_style = effective_config->timestamp_style;
  out[0] = '\0';
  {
    char ts[80];
    _nei_log_format_timestamp(header->timestamp_ns, ts_style, ts, sizeof(ts));
    if (ts[0] != '\0') {
      if (_nei_log_append_char(out, out_cap, &used, '[') != 0)
        return -1;
      if (_nei_log_append_cstr(out, out_cap, &used, ts) != 0)
        return -1;
      if (_nei_log_append_cstr(out, out_cap, &used, "] ") != 0)
        return -1;
    }
  }

  if (header->verbose == _NEI_LOG_NOT_VERBOSE) {
    if (_nei_log_append_char(out, out_cap, &used, '[') != 0)
      return -1;
    if (_nei_log_append_cstr(out,
                             out_cap,
                             &used,
                             short_level_tag ? _get_level_short_string((nei_log_level_e)header->level)
                                             : _get_level_string((nei_log_level_e)header->level))
        != 0)
      return -1;
    if (_nei_log_append_cstr(out, out_cap, &used, "] ") != 0)
      return -1;
  } else {
    if (_nei_log_append_cstr(out, out_cap, &used, "[V] ") != 0)
      return -1;
  }
  if (header->thread_id_len > 0U) {
    if (_nei_log_append_cstr(out, out_cap, &used, "tid=") != 0)
      return -1;
    if (_nei_log_append_nstr(out, out_cap, &used, header->thread_id_str, (size_t)header->thread_id_len) != 0)
      return -1;
    if (_nei_log_append_cstr(out, out_cap, &used, " ") != 0)
      return -1;
  }
  if (log_location != 0 && log_location_after_message == 0) {
    if (_nei_log_append_location_block(header, short_path, out, out_cap, &used) != 0)
      return -1;
    if ((header->file_ptr != NULL || header->func_ptr != NULL) && _nei_log_append_cstr(out, out_cap, &used, " - ") != 0)
      return -1;
  }

  if (header->fmt_ptr == NULL) {
    if (cursor >= end) {
      return -1;
    }
    if (*cursor != _NEI_LOG_PAYLOAD_LITERAL_MSG) {
      return -1;
    }
    ++cursor;
    {
      uint16_t len16 = 0;
      if (_nei_log_read_u16(&cursor, end, &len16) != 0) {
        return -1;
      }
      if (len16 > _NEI_LOG_MAX_STRING_COPY || cursor + len16 > end) {
        return -1;
      }
      if (len16 > 0U && _nei_log_append_nstr(out, out_cap, &used, (const char *)cursor, len16) != 0) {
        return -1;
      }
    }
    if (log_location != 0 && log_location_after_message != 0
        && (header->file_ptr != NULL || header->func_ptr != NULL)) {
      if (_nei_log_append_cstr(out, out_cap, &used, " - ") != 0)
        return -1;
      if (_nei_log_append_location_block(header, short_path, out, out_cap, &used) != 0)
        return -1;
    }
    return 0;
  }

  fmt = header->fmt_ptr;
  {
    int has_percent = 0;
    int simple_replay = 0;
    size_t fmt_len = 0U;
    _nei_log_get_fmt_meta_cached(fmt, &has_percent, &fmt_len, &simple_replay);
    if (has_percent == 0) {
      if (fmt_len > 0U && _nei_log_append_nstr(out, out_cap, &used, fmt, fmt_len) != 0) {
        return -1;
      }
      if (log_location != 0 && log_location_after_message != 0 && (header->file_ptr != NULL || header->func_ptr != NULL)) {
        if (_nei_log_append_cstr(out, out_cap, &used, " - ") != 0)
          return -1;
        if (_nei_log_append_location_block(header, short_path, out, out_cap, &used) != 0)
          return -1;
      }
      return 0;
    }
    if (simple_replay != 0) {
      if (_nei_log_format_simple_replay(fmt, &cursor, end, out, out_cap, &used) != 0) {
        return -1;
      }
      if (log_location != 0 && log_location_after_message != 0 && (header->file_ptr != NULL || header->func_ptr != NULL)) {
        if (_nei_log_append_cstr(out, out_cap, &used, " - ") != 0)
          return -1;
        if (_nei_log_append_location_block(header, short_path, out, out_cap, &used) != 0)
          return -1;
      }
      return 0;
    }
  }
  scan = fmt;
  while (*scan) {
    if (*scan != '%') {
      if (_nei_log_append_char(out, out_cap, &used, *scan) != 0)
        return -1;
      ++scan;
      continue;
    }
    {
      char conv_spec[64];
      const char *after = NULL;
      if (_nei_log_build_runtime_conversion_spec(scan, &after, &cursor, end, &parsed_args, conv_spec, sizeof(conv_spec))
          != 0) {
        return -1;
      }
      if (strcmp(conv_spec, "%%") == 0) {
        if (_nei_log_append_char(out, out_cap, &used, '%') != 0)
          return -1;
        scan = after;
        continue;
      }
      if (cursor >= end) {
        return -1;
      }
      {
        const uint8_t payload_type = *cursor++;
        if (_nei_log_is_simple_spec(conv_spec)) {
          if (_nei_log_append_from_spec(conv_spec, payload_type, &cursor, end, out, out_cap, &used) != 0) {
            return -1;
          }
          scan = after;
          continue;
        }
        char tmp[256];
        switch (payload_type) {
        case _NEI_LOG_PAYLOAD_I32: {
          int32_t v = 0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          snprintf(tmp, sizeof(tmp), conv_spec, (int)v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_U32: {
          uint32_t v = 0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          snprintf(tmp, sizeof(tmp), conv_spec, (unsigned int)v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_I64: {
          int64_t v = 0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          _nei_log_snprintf_i64(conv_spec, tmp, sizeof(tmp), v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_U64: {
          uint64_t v = 0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          _nei_log_snprintf_u64(conv_spec, tmp, sizeof(tmp), v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_DOUBLE: {
          double v = 0.0;
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          snprintf(tmp, sizeof(tmp), conv_spec, v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_LONGDOUBLE: {
          uint8_t raw[_NEI_LOG_LONGDOUBLE_STORAGE];
          long double ld = 0.0L;
          if (_nei_log_read_bytes(&cursor, end, raw, sizeof(raw)) != 0)
            return -1;
          memset(&ld, 0, sizeof(ld));
          memcpy(&ld, raw, sizeof(ld) < sizeof(raw) ? sizeof(ld) : sizeof(raw));
          snprintf(tmp, sizeof(tmp), conv_spec, ld);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_CHAR: {
          char v = '\0';
          if (_nei_log_read_bytes(&cursor, end, &v, sizeof(v)) != 0)
            return -1;
          snprintf(tmp, sizeof(tmp), conv_spec, (int)v);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_PTR: {
          uintptr_t raw = 0U;
          void *ptr = NULL;
          if (_nei_log_read_bytes(&cursor, end, &raw, sizeof(raw)) != 0)
            return -1;
          ptr = (void *)raw;
          snprintf(tmp, sizeof(tmp), conv_spec, ptr);
          if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
            return -1;
          break;
        }
        case _NEI_LOG_PAYLOAD_CSTR: {
          uint16_t len16 = 0;
          if (_nei_log_read_u16(&cursor, end, &len16) != 0)
            return -1;
          if (len16 > _NEI_LOG_MAX_STRING_COPY || cursor + len16 > end)
            return -1;
          if (conv_spec[0] == '%' && conv_spec[1] == 's' && conv_spec[2] == '\0') {
            if (len16 > 0U && _nei_log_append_nstr(out, out_cap, &used, (const char *)cursor, (size_t)len16) != 0) {
              return -1;
            }
            cursor += len16;
            break;
          }
          {
            char strbuf[_NEI_LOG_MAX_STRING_COPY + 1U];
            memcpy(strbuf, cursor, len16);
            strbuf[len16] = '\0';
            cursor += len16;
            snprintf(tmp, sizeof(tmp), conv_spec, strbuf);
            if (_nei_log_append_cstr(out, out_cap, &used, tmp) != 0)
              return -1;
          }
          break;
        }
        default:
          return -1;
        }
        ++parsed_args;
      }
      scan = after;
      continue;
    }
  }

  if (log_location != 0 && log_location_after_message != 0 && (header->file_ptr != NULL || header->func_ptr != NULL)) {
    if (_nei_log_append_cstr(out, out_cap, &used, " - ") != 0)
      return -1;
    if (_nei_log_append_location_block(header, short_path, out, out_cap, &used) != 0)
      return -1;
  }
  return 0;
}

#pragma endregion
