#include "log_internal.h"

#pragma region sink implementation

static uint32_t _nei_log_parse_env_u32(const char *name, uint32_t fallback) {
  const char *s = getenv(name);
  char *end = NULL;
  unsigned long v = 0UL;
  if (s == NULL || s[0] == '\0') {
    return fallback;
  }
  v = strtoul(s, &end, 10);
  if (end == s || (end != NULL && *end != '\0') || v > 0xFFFFFFFFUL) {
    return fallback;
  }
  return (uint32_t)v;
}

static void _nei_log_file_write_line(FILE *fp, const char *message, size_t length);

static void _nei_log_file_sink_flush_pending(nei_log_default_file_sink_ctx_st *ctx, int flush_stream) {
  size_t written;
  if (ctx == NULL || ctx->fp == NULL) {
    return;
  }
  if (ctx->write_batch_used > 0U) {
    written = fwrite(ctx->write_batch_buf, 1U, ctx->write_batch_used, ctx->fp);
    (void)written;
    ctx->write_batch_used = 0U;
  }
  if (flush_stream != 0) {
    (void)fflush(ctx->fp);
  }
}

static void _nei_log_file_sink_append_line(nei_log_default_file_sink_ctx_st *ctx, const char *message, size_t length) {
  const size_t line_size = length + 1U;
  if (ctx == NULL || ctx->fp == NULL || message == NULL) {
    return;
  }

  if (ctx->write_batch_buf == NULL || ctx->write_batch_cap == 0U) {
    _nei_log_file_write_line(ctx->fp, message, length);
    return;
  }

  if (line_size > ctx->write_batch_cap) {
    _nei_log_file_sink_flush_pending(ctx, 0);
    _nei_log_file_write_line(ctx->fp, message, length);
    return;
  }

  if (ctx->write_batch_used + line_size > ctx->write_batch_cap) {
    _nei_log_file_sink_flush_pending(ctx, 0);
  }

  memcpy(ctx->write_batch_buf + ctx->write_batch_used, message, length);
  ctx->write_batch_used += length;
  ctx->write_batch_buf[ctx->write_batch_used] = '\n';
  ctx->write_batch_used += 1U;
}

static void _nei_log_file_write_line(FILE *fp, const char *message, size_t length) {
  if (fp == NULL || message == NULL) {
    return;
  }

  /* Fast path for short lines: one fwrite call avoids extra libc overhead. */
  if (length <= 1023U) {
    char line[1024 + 1];
    memcpy(line, message, length);
    line[length] = '\n';
    (void)fwrite(line, 1U, length + 1U, fp);
    return;
  }

  (void)fwrite(message, 1U, length, fp);
  (void)fputc('\n', fp);
}

void _nei_log_default_file_llog(const nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length) {
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  (void)level;
  if (sink == NULL || message == NULL) {
    return;
  }
  ctx = (nei_log_default_file_sink_ctx_st *)sink->opaque;
  if (ctx == NULL || ctx->magic != _NEI_LOG_DEFAULT_FILE_SINK_MAGIC || ctx->fp == NULL) {
    return;
  }
  _nei_log_file_sink_append_line(ctx, message, length);
  /* Batch flush: only fflush after every N logs (default 256). */
  if (ctx->flush_interval > 0U) {
    ctx->flush_counter++;
    if (ctx->flush_counter >= ctx->flush_interval) {
      ctx->flush_counter = 0U;
      _nei_log_file_sink_flush_pending(ctx, 1);
    }
  } else {
    _nei_log_file_sink_flush_pending(ctx, 1);
  }
}

void _nei_log_default_file_vlog(const nei_log_sink_st *sink, int verbose, const char *message, size_t length) {
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  if (sink == NULL || message == NULL) {
    return;
  }
  ctx = (nei_log_default_file_sink_ctx_st *)sink->opaque;
  if (ctx == NULL || ctx->magic != _NEI_LOG_DEFAULT_FILE_SINK_MAGIC || ctx->fp == NULL) {
    return;
  }
  (void)verbose;
  _nei_log_file_sink_append_line(ctx, message, length);
  /* Batch flush: only fflush after every N logs (default 256). */
  if (ctx->flush_interval > 0U) {
    ctx->flush_counter++;
    if (ctx->flush_counter >= ctx->flush_interval) {
      ctx->flush_counter = 0U;
      _nei_log_file_sink_flush_pending(ctx, 1);
    }
  } else {
    _nei_log_file_sink_flush_pending(ctx, 1);
  }
}

void _nei_log_emit_message(const nei_log_config_st *config, int32_t level, int32_t verbose, const char *message, size_t length) {
  const nei_log_config_st *effective = config;
  size_t i;
  if (config == NULL || message == NULL) {
    return;
  }
  if (verbose == _NEI_LOG_NOT_VERBOSE) {
    const uint32_t mask = (uint32_t)(1U << (uint32_t)level);
    if (level < (int32_t)NEI_L_VERBOSE || level > (int32_t)NEI_L_FATAL) {
      return;
    }
    if ((effective->level_flags.all & mask) == 0U) {
      return;
    }
  } else if (effective->verbose_threshold >= 0 && verbose > effective->verbose_threshold) {
    return;
  }
  for (i = 0; i < NEI_LOG_MAX_SINKS_OF_CONFIG; ++i) {
    nei_log_sink_st *sink = effective->sinks[i];
    if (sink == NULL) {
      break;
    }
    if (verbose != _NEI_LOG_NOT_VERBOSE) {
      if (sink->vlog != NULL) {
        sink->vlog(sink, verbose, message, length);
      }
    } else {
      if (sink->llog != NULL) {
        sink->llog(sink, (nei_log_level_e)level, message, length);
      }
    }
  }
  if (effective->log_to_console) {
    (void)fwrite(message, 1U, length, stdout);
    (void)fputc('\n', stdout);
  }
}

#pragma endregion

#pragma region public API

nei_log_sink_st *nei_log_create_default_file_sink(const char *filename) {
  nei_log_sink_st *sink = NULL;
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  FILE *fp = NULL;
  uint32_t flush_interval = 0U;
  uint32_t file_buffer_bytes = 0U;
  uint32_t write_batch_bytes = 0U;

  if (filename == NULL || filename[0] == '\0') {
    return NULL;
  }

#if defined(_WIN32)
  if (fopen_s(&fp, filename, "ab") != 0) {
    fp = NULL;
  }
#else
  fp = fopen(filename, "ab");
#endif
  if (fp == NULL) {
    return NULL;
  }

  flush_interval = _nei_log_parse_env_u32("NEI_LOG_FILE_FLUSH_INTERVAL", 256U);
  file_buffer_bytes = _nei_log_parse_env_u32("NEI_LOG_FILE_BUFFER_BYTES", 1024U * 1024U);
  write_batch_bytes = _nei_log_parse_env_u32("NEI_LOG_FILE_WRITE_BATCH_BYTES", 64U * 1024U);
  if (file_buffer_bytes > 0U) {
    (void)setvbuf(fp, NULL, _IOFBF, (size_t)file_buffer_bytes);
  }

  sink = (nei_log_sink_st *)calloc(1U, sizeof(*sink));
  ctx = (nei_log_default_file_sink_ctx_st *)calloc(1U, sizeof(*ctx));
  if (sink == NULL || ctx == NULL) {
    if (fp != NULL) {
      fclose(fp);
    }
    free(ctx);
    free(sink);
    return NULL;
  }

  ctx->magic = _NEI_LOG_DEFAULT_FILE_SINK_MAGIC;
  ctx->fp = fp;
  ctx->flush_counter = 0U;
  ctx->flush_interval = flush_interval; /* default 256; 0 means always fflush. */
  ctx->write_batch_buf = NULL;
  ctx->write_batch_cap = 0U;
  ctx->write_batch_used = 0U;
  if (write_batch_bytes > 0U) {
    ctx->write_batch_buf = (char *)malloc((size_t)write_batch_bytes);
    if (ctx->write_batch_buf != NULL) {
      ctx->write_batch_cap = (size_t)write_batch_bytes;
    }
  }

  sink->llog = _nei_log_default_file_llog;
  sink->vlog = _nei_log_default_file_vlog;
  sink->opaque = ctx;
  return sink;
}

void nei_log_destroy_sink(nei_log_sink_st *sink) {
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  if (sink == NULL) {
    return;
  }

  if (sink->opaque != NULL && sink->llog == _nei_log_default_file_llog && sink->vlog == _nei_log_default_file_vlog) {
    ctx = (nei_log_default_file_sink_ctx_st *)sink->opaque;
    if (ctx->magic == _NEI_LOG_DEFAULT_FILE_SINK_MAGIC) {
      if (ctx->fp != NULL) {
        _nei_log_file_sink_flush_pending(ctx, 1);
        fclose(ctx->fp);
      }
      if (ctx->write_batch_buf != NULL) {
        free(ctx->write_batch_buf);
      }
      ctx->write_batch_buf = NULL;
      ctx->write_batch_cap = 0U;
      ctx->write_batch_used = 0U;
      ctx->fp = NULL;
      ctx->magic = 0U;
      free(ctx);
    }
  }
  free(sink);
}

#pragma endregion
