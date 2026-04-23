#include "log_internal.h"

#pragma region sink implementation

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
  (void)fwrite(message, 1U, length, ctx->fp);
  (void)fputc('\n', ctx->fp);
  /* Batch flush: only fflush after every N logs (default 100). */
  if (ctx->flush_interval > 0U) {
    ctx->flush_counter++;
    if (ctx->flush_counter >= ctx->flush_interval) {
      ctx->flush_counter = 0U;
      (void)fflush(ctx->fp);
    }
  } else {
    (void)fflush(ctx->fp);
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
  (void)fwrite(message, 1U, length, ctx->fp);
  (void)fputc('\n', ctx->fp);
  /* Batch flush: only fflush after every N logs (default 100). */
  if (ctx->flush_interval > 0U) {
    ctx->flush_counter++;
    if (ctx->flush_counter >= ctx->flush_interval) {
      ctx->flush_counter = 0U;
      (void)fflush(ctx->fp);
    }
  } else {
    (void)fflush(ctx->fp);
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
  ctx->flush_interval = 100U; /* Flush every 100 logs by default. */

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
        fclose(ctx->fp);
      }
      ctx->fp = NULL;
      ctx->magic = 0U;
      free(ctx);
    }
  }
  free(sink);
}

#pragma endregion
