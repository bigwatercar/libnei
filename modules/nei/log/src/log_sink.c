#include "log_internal.h"

#if !defined(_WIN32)
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#pragma region sink implementation

static void _nei_log_file_write_line(FILE *fp, const char *message, size_t length);
static void _nei_log_file_sink_flush_pending(nei_log_default_file_sink_ctx_st *ctx, int flush_stream);
static void _nei_log_file_sink_configure_stream_buffer(FILE *fp, uint32_t file_buffer_bytes);

#if !defined(_WIN32)
static int _nei_log_file_write_all_fd(int fd, const char *data, size_t length) {
  while (length > 0U) {
    const ssize_t written = write(fd, data, length);
    if (written > 0) {
      data += (size_t)written;
      length -= (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
  return 0;
}

static int _nei_log_file_writev_all_fd(int fd, const char *message, size_t length) {
  struct iovec iov[2];
  const char newline = '\n';
  int iov_count = 2;

  iov[0].iov_base = (void *)message;
  iov[0].iov_len = length;
  iov[1].iov_base = (void *)&newline;
  iov[1].iov_len = 1U;

  while (iov_count > 0) {
    const ssize_t written = writev(fd, iov, iov_count);
    if (written > 0) {
      size_t remaining = (size_t)written;
      int index = 0;
      while (index < iov_count && remaining > 0U) {
        if (remaining >= iov[index].iov_len) {
          remaining -= iov[index].iov_len;
          ++index;
        } else {
          iov[index].iov_base = (char *)iov[index].iov_base + remaining;
          iov[index].iov_len -= remaining;
          remaining = 0U;
        }
      }
      if (index > 0) {
        int dst = 0;
        while (index < iov_count) {
          iov[dst++] = iov[index++];
        }
        iov_count = dst;
      }
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
  return 0;
}
#endif

static FILE *_nei_log_open_default_file_sink_file(const char *filename) {
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
  return fp;
}

static void _nei_log_file_sink_configure_stream_buffer(FILE *fp, uint32_t file_buffer_bytes) {
  if (fp == NULL) {
    return;
  }
  if (file_buffer_bytes > 0U) {
    (void)setvbuf(fp, NULL, _IOFBF, (size_t)file_buffer_bytes);
  } else {
    (void)setvbuf(fp, NULL, _IONBF, 0U);
  }
}

static size_t _nei_log_file_sink_measure_size(FILE *fp) {
  long pos;
  if (fp == NULL) {
    return 0U;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    return 0U;
  }
  pos = ftell(fp);
  if (pos < 0L) {
    return 0U;
  }
  return (size_t)pos;
}

static char *_nei_log_file_sink_dup_filename(const char *filename) {
  const size_t len = strlen(filename);
  char *copy = (char *)malloc(len + 1U);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, filename, len + 1U);
  return copy;
}

static int _nei_log_file_sink_build_rotated_path(const char *filename,
                                                 uint32_t index,
                                                 char *out,
                                                 size_t out_cap) {
  int written;
  if (filename == NULL || out == NULL || out_cap == 0U) {
    return -1;
  }
  written = snprintf(out, out_cap, "%s.%u", filename, (unsigned)index);
  if (written < 0 || (size_t)written >= out_cap) {
    return -1;
  }
  return 0;
}

static int _nei_log_file_sink_rotate(nei_log_default_file_sink_ctx_st *ctx) {
  uint32_t index;
  size_t path_cap;
  char *src_path = NULL;
  char *dst_path = NULL;
  FILE *new_fp;

  if (ctx == NULL || ctx->fp == NULL || ctx->filename == NULL || ctx->max_backup_files == 0U) {
    return -1;
  }

  _nei_log_file_sink_flush_pending(ctx, 1);
  fclose(ctx->fp);
  ctx->fp = NULL;
  ctx->write_batch_used = 0U;
  ctx->flush_counter = 0U;

  path_cap = strlen(ctx->filename) + 16U;
  src_path = (char *)malloc(path_cap);
  dst_path = (char *)malloc(path_cap);
  if (src_path == NULL || dst_path == NULL) {
    free(src_path);
    free(dst_path);
    return -1;
  }

  if (_nei_log_file_sink_build_rotated_path(ctx->filename, ctx->max_backup_files, dst_path, path_cap) == 0) {
    (void)remove(dst_path);
  }

  for (index = ctx->max_backup_files; index > 1U; --index) {
    if (_nei_log_file_sink_build_rotated_path(ctx->filename, index - 1U, src_path, path_cap) != 0 ||
        _nei_log_file_sink_build_rotated_path(ctx->filename, index, dst_path, path_cap) != 0) {
      continue;
    }
    (void)remove(dst_path);
    (void)rename(src_path, dst_path);
  }

  if (_nei_log_file_sink_build_rotated_path(ctx->filename, 1U, dst_path, path_cap) == 0) {
    (void)remove(dst_path);
    (void)rename(ctx->filename, dst_path);
  }

  free(src_path);
  free(dst_path);

  new_fp = _nei_log_open_default_file_sink_file(ctx->filename);
  if (new_fp == NULL) {
    return -1;
  }
  _nei_log_file_sink_configure_stream_buffer(new_fp, ctx->file_buffer_bytes);
  ctx->fp = new_fp;
  ctx->current_size = _nei_log_file_sink_measure_size(new_fp);
  return 0;
}

static void _nei_log_file_sink_prepare_write(nei_log_default_file_sink_ctx_st *ctx, size_t line_size) {
  if (ctx == NULL || ctx->fp == NULL || ctx->max_file_bytes == 0U || ctx->max_backup_files == 0U) {
    return;
  }
  if (ctx->current_size == 0U) {
    return;
  }
  if (ctx->current_size + line_size <= ctx->max_file_bytes) {
    return;
  }
  (void)_nei_log_file_sink_rotate(ctx);
}

static int _nei_log_file_sink_is_immediate_flush(const nei_log_default_file_sink_ctx_st *ctx) {
  return ctx != NULL && (ctx->flush_interval <= 1U) && (ctx->write_batch_buf == NULL || ctx->write_batch_cap == 0U);
}

static void _nei_log_file_sink_write_line_and_flush(nei_log_default_file_sink_ctx_st *ctx,
                                                    const char *message,
                                                    size_t length) {
  const size_t line_size = length + 1U;
  if (ctx == NULL || ctx->fp == NULL || message == NULL) {
    return;
  }
  _nei_log_file_sink_prepare_write(ctx, line_size);
  _nei_log_file_write_line(ctx->fp, message, length);
  ctx->current_size += line_size;
  (void)fflush(ctx->fp);
}

static void _nei_log_file_sink_flush_pending(nei_log_default_file_sink_ctx_st *ctx, int flush_stream) {
  if (ctx == NULL || ctx->fp == NULL) {
    return;
  }
  if (ctx->write_batch_used > 0U) {
#if !defined(_WIN32)
    (void)_nei_log_file_write_all_fd(fileno(ctx->fp), ctx->write_batch_buf, ctx->write_batch_used);
#else
    size_t written = fwrite(ctx->write_batch_buf, 1U, ctx->write_batch_used, ctx->fp);
    (void)written;
#endif
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

  _nei_log_file_sink_prepare_write(ctx, line_size);

  if (ctx->write_batch_buf == NULL || ctx->write_batch_cap == 0U) {
    _nei_log_file_write_line(ctx->fp, message, length);
    ctx->current_size += line_size;
    return;
  }

  if (line_size > ctx->write_batch_cap) {
    _nei_log_file_sink_flush_pending(ctx, 0);
    _nei_log_file_write_line(ctx->fp, message, length);
    ctx->current_size += line_size;
    return;
  }

  if (ctx->write_batch_used + line_size > ctx->write_batch_cap) {
    _nei_log_file_sink_flush_pending(ctx, 0);
  }

  memcpy(ctx->write_batch_buf + ctx->write_batch_used, message, length);
  ctx->write_batch_used += length;
  ctx->write_batch_buf[ctx->write_batch_used] = '\n';
  ctx->write_batch_used += 1U;
  ctx->current_size += line_size;
}

static void _nei_log_file_write_line(FILE *fp, const char *message, size_t length) {
  if (fp == NULL || message == NULL) {
    return;
  }

#if !defined(_WIN32)
  {
    const int fd = fileno(fp);
    if (fd >= 0) {
      (void)_nei_log_file_writev_all_fd(fd, message, length);
      return;
    }
  }
#endif

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
  if (_nei_log_file_sink_is_immediate_flush(ctx)) {
    _nei_log_file_sink_write_line_and_flush(ctx, message, length);
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
  if (_nei_log_file_sink_is_immediate_flush(ctx)) {
    _nei_log_file_sink_write_line_and_flush(ctx, message, length);
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

#define _NEI_LOG_DEFAULT_FLUSH_INTERVAL    256U
#define _NEI_LOG_DEFAULT_FILE_BUFFER_BYTES  (1024U * 1024U)
#define _NEI_LOG_DEFAULT_WRITE_BATCH_BYTES  (64U * 1024U)

nei_log_default_file_sink_options_st nei_log_default_file_sink_options(void) {
  nei_log_default_file_sink_options_st opts;
  opts.max_file_bytes    = 0U;
  opts.max_backup_files  = 0U;
  opts.flush_interval    = _NEI_LOG_DEFAULT_FLUSH_INTERVAL;
  opts.file_buffer_bytes = _NEI_LOG_DEFAULT_FILE_BUFFER_BYTES;
  opts.write_batch_bytes = _NEI_LOG_DEFAULT_WRITE_BATCH_BYTES;
  return opts;
}

nei_log_sink_st *nei_log_create_default_file_sink(const char *filename,
                                                  const nei_log_default_file_sink_options_st *options) {
  nei_log_sink_st *sink = NULL;
  nei_log_default_file_sink_ctx_st *ctx = NULL;
  FILE *fp = NULL;
  uint32_t flush_interval = 0U;
  uint32_t file_buffer_bytes = 0U;
  uint32_t write_batch_bytes = 0U;
  size_t max_file_bytes = 0U;
  uint32_t max_backup_files = 0U;

  if (filename == NULL || filename[0] == '\0') {
    return NULL;
  }

  if (options != NULL) {
    flush_interval    = options->flush_interval;
    file_buffer_bytes = options->file_buffer_bytes;
    write_batch_bytes = options->write_batch_bytes;
    max_file_bytes    = options->max_file_bytes;
    max_backup_files  = options->max_backup_files;
  } else {
    flush_interval    = _NEI_LOG_DEFAULT_FLUSH_INTERVAL;
    file_buffer_bytes = _NEI_LOG_DEFAULT_FILE_BUFFER_BYTES;
    write_batch_bytes = _NEI_LOG_DEFAULT_WRITE_BATCH_BYTES;
  }

  fp = _nei_log_open_default_file_sink_file(filename);
  if (fp == NULL) {
    return NULL;
  }

  if (write_batch_bytes > 0U) {
    file_buffer_bytes = 0U;
  }
  _nei_log_file_sink_configure_stream_buffer(fp, file_buffer_bytes);

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
  ctx->flush_interval = flush_interval; /* 0 means always fflush; default is 256. */
  ctx->file_buffer_bytes = file_buffer_bytes;
  ctx->filename = _nei_log_file_sink_dup_filename(filename);
  ctx->max_backup_files = max_backup_files;
  ctx->write_batch_buf = NULL;
  ctx->current_size = _nei_log_file_sink_measure_size(fp);
  ctx->max_file_bytes = max_file_bytes;
  ctx->write_batch_cap = 0U;
  ctx->write_batch_used = 0U;
  if (ctx->filename == NULL) {
    fclose(fp);
    free(ctx);
    free(sink);
    return NULL;
  }
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
      if (ctx->filename != NULL) {
        free(ctx->filename);
      }
      ctx->filename = NULL;
      ctx->max_backup_files = 0U;
      ctx->write_batch_buf = NULL;
      ctx->current_size = 0U;
      ctx->max_file_bytes = 0U;
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
