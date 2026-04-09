/**
 * @file log.h
 * @author ylf
 * @brief NEI Logger
 * @version 0.1
 * @date 2025-08-30
 *
 * @copyright Copyright (c) 2025
 */
#pragma once
#ifndef NEI_LOG_LOG_H
#define NEI_LOG_LOG_H

#include <nei/macros/nei_export.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup nei_log NEI Log
 * @brief C logging API and utilities.
 * @{
 */

/**
 * @defgroup nei_log_internal Internal helpers
 * @ingroup nei_log
 * @brief Internal macros and helpers used by the logging API.
 * @{
 */

/**
 * @brief Get current function name/signature
 * @details Used for logging function signature/name. Different compilers use
 * different built-ins.
 */
#undef NEI_FUNC
#if defined(_MSC_VER)
#define NEI_FUNC __FUNCSIG__
#elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#define NEI_FUNC __PRETTY_FUNCTION__
#else
#if defined(__func__)
#define NEI_FUNC __func__
#else
#define NEI_FUNC ""
#endif
#endif

/** @} */ /* end of nei_log_internal */

/**
 * @defgroup nei_log_types Types
 * @ingroup nei_log
 * @brief Public types used by the logging API (config-related types first,
 * then sink types).
 * @{
 */

/* --- Config-related types --- */

/**
 * @brief Opaque configuration handle
 * @details Pointer-width integer token returned by the logging library.
 * Treat as opaque; do not infer internal layout or perform arithmetic.
 */
typedef uintptr_t nei_log_config_handle_t;
#define NEI_LOG_INVALID_CONFIG_HANDLE ((nei_log_config_handle_t)0u)
#define NEI_LOG_DEFAULT_CONFIG_HANDLE ((nei_log_config_handle_t)1u)

/// @brief Maximum number of sinks in a configuration
#define NEI_LOG_MAX_SINKS_OF_CONFIG 8

/**
 * @brief Log level
 * @details From low to high. Typically used for filtering and tagging output.
 */
typedef enum nei_log_level_e {
  NEI_LOG_LEVEL_VERBOSE,
  NEI_LOG_LEVEL_TRACE,
  NEI_LOG_LEVEL_DEBUG,
  NEI_LOG_LEVEL_INFO,
  NEI_LOG_LEVEL_WARN,
  NEI_LOG_LEVEL_ERROR,
  NEI_LOG_LEVEL_FATAL,
} nei_log_level_e;

/**
 * @brief Log level flags
 * @details Bitfield switches for each level. You can also set/read the full
 * mask via
 * @ref nei_log_level_flags_u::all.
 */
typedef union nei_log_level_flags_u {
  struct {
    uint32_t verbose : 1;
    uint32_t trace : 1;
    uint32_t debug : 1;
    uint32_t info : 1;
    uint32_t warn : 1;
    uint32_t error : 1;
    uint32_t fatal : 1;
  } flags;

  uint32_t all;
} nei_log_level_flags_u;

typedef struct nei_log_sink_st nei_log_sink_st;

/**
 * @brief Log configuration
 *
 * @details
 * - @c level_flags: Per-level switches; multiple levels may be enabled
 * simultaneously. Applied when dispatching to sinks and console.
 * - @c verbose_threshold: Maximum verbose sub-level emitted to sinks and
 * console; messages with a greater sub-level are dropped. A value @c < 0
 * disables verbose filtering (all verbose levels are emitted).
 * - @c short_level_tag: Whether to use short level tags
 * - @c short_path: Whether to use a short file path (file name only, no
 * directories)
 * - @c datetime_format: Format string passed to @c strftime for the
 * timestamp; milliseconds may be appended when output space allows.
 * - @c sinks: Registered sinks in array order. Dispatch stops at the first
 * NULL entry, or after @ref NEI_LOG_MAX_SINKS_OF_CONFIG non-NULL entries. Do
 * not place NULL between active sinks.
 */
typedef struct nei_log_config_st {
  nei_log_level_flags_u level_flags;
  int verbose_threshold;
  int short_level_tag;
  int short_path;
  int log_to_console;
  const char *datetime_format;
  nei_log_sink_st *sinks[NEI_LOG_MAX_SINKS_OF_CONFIG];
} nei_log_config_st;

/* --- Sink types --- */

/**
 * @brief Log sink
 *
 * @details By registering one or more sinks in @ref nei_log_config_st::sinks,
 * you can route logs to different destinations (e.g., stdout, files, syslog,
 * network, etc.).
 */
typedef struct nei_log_sink_st {
  /** @brief Callback for level-based logs (can be NULL). */
  void (*llog)(const struct nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length);
  /** @brief Callback for verbose logs (can be NULL). */
  void (*vlog)(const struct nei_log_sink_st *sink, int verbose, const char *message, size_t length);
  /** @brief User data pointer; lifetime is managed by the caller. */
  void *opaque;
} nei_log_sink_st;

/**
 * @brief Log sink callback type for level-based logs
 *
 * @param sink Log sink pointer. Use @ref nei_log_sink_st::opaque for
 * sink-specific state and filtering when needed.
 * @param level Log level
 * @param message Message buffer (not guaranteed to be '\\0'-terminated)
 * @param length Message length in bytes
 */
typedef void (*nei_pfn_llog)(const nei_log_sink_st *sink, nei_log_level_e level, const char *message, size_t length);

/**
 * @brief Log sink callback type for verbose logs
 *
 * @param sink Log sink pointer. Use @ref nei_log_sink_st::opaque for
 * sink-specific state and filtering when needed.
 * @param verbose Verbose sub-level (for finer-grained verbose output)
 * @param message Message buffer (not guaranteed to be '\\0'-terminated)
 * @param length Message length in bytes
 */
typedef void (*nei_pfn_vlog)(const nei_log_sink_st *sink, int verbose, const char *message, size_t length);

/** @} */ /* end of nei_log_types */

/**
 * @defgroup nei_log_api_config Configuration API
 * @ingroup nei_log
 * @brief Configuration table and default config.
 * @{
 */

/**
 * @brief Get a log configuration by handle
 *
 * @param[in] handle Configuration handle
 * @return Pointer to the library-owned configuration, or NULL if not found.
 * The object may be modified in place (same caveats as
 * @ref nei_log_default_config regarding thread safety).
 */
NEI_API nei_log_config_st *nei_log_get_config(nei_log_config_handle_t handle);

/**
 * @brief Get the default log configuration
 * @details Returns the default configuration (slot 0 of the internal config
 * table, handle @ref NEI_LOG_DEFAULT_CONFIG_HANDLE).
 *
 * The returned pointer is managed by the logging library and remains valid for
 * the lifetime of the process. Callers may override fields in-place (e.g.
 * `sinks`, formatting options); such changes are not guaranteed to be
 * thread-safe.
 *
 * @return Pointer to the default configuration.
 */
NEI_API nei_log_config_st *nei_log_default_config(void);

/**
 * @brief Add a log configuration and return its handle
 *
 * @param[in] config Configuration to add. The logging library will copy the
 * configuration; ownership of @p config is not transferred.
 * @param[out] out_handle Returned handle (optional; can be NULL)
 *
 * @return @c 0 on success, @c -1 on failure (NULL @p config or config table full)
 */
NEI_API int nei_log_add_config(const nei_log_config_st *config, nei_log_config_handle_t *out_handle);

/**
 * @brief Remove a log configuration by handle
 *
 * @param[in] handle Configuration handle
 */
NEI_API void nei_log_remove_config(nei_log_config_handle_t handle);

/** @} */ /* end of nei_log_api_config */

/**
 * @defgroup nei_log_api_sink Sink API
 * @ingroup nei_log
 * @brief Built-in sinks.
 * @{
 */

/**
 * @brief Create a built-in file sink.
 *
 * @param[in] filename Output file path (opened in append-binary mode).
 * @return Heap-allocated sink pointer, or NULL on failure.
 *
 * @note Level and verbose filtering are controlled by the owning
 * @ref nei_log_config_st (see @ref nei_log_config_st::level_flags and
 * @ref nei_log_config_st::verbose_threshold). Custom per-sink state should use
 * @ref nei_log_sink_st::opaque on a sink you own; release the sink with
 * @ref nei_log_destroy_sink.
 */
NEI_API nei_log_sink_st *nei_log_create_default_file_sink(const char *filename);

/**
 * @brief Destroy a log sink structure allocated by the library (e.g.
 * @ref nei_log_create_default_file_sink).
 *
 * @param[in] sink Sink pointer to destroy. NULL is allowed.
 *
 * @note For the default file sink, @ref nei_log_sink_st::opaque is closed and
 * freed. For custom sinks, release @p opaque yourself first if needed; this
 * routine always @c free()s the @ref nei_log_sink_st itself.
 */
NEI_API void nei_log_destroy_sink(nei_log_sink_st *sink);

/** @} */ /* end of nei_log_api_sink */

/**
 * @defgroup nei_log_functions Log API
 * @ingroup nei_log
 * @brief Record emission and flush.
 * @{
 */

#if defined(_MSC_VER)
#define PRINTF_LIKE(fmtIndex, vaIndex)
#elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#define PRINTF_LIKE(fmtIndex, vaIndex) __attribute__((format(printf, fmtIndex, vaIndex)))
#else
#define PRINTF_LIKE(fmtIndex, vaIndex)
#endif

/**
 * @brief Write a level-based log entry
 *
 * @param[in] config_handle Log configuration handle (typically @ref NEI_LOG_DEFAULT_CONFIG_HANDLE)
 * @param[in] level Log level
 * @param[in] file Source file path (typically @c __FILE__)
 * @param[in] line Source line number (typically @c __LINE__)
 * @param[in] func Function signature/name (typically @ref NEI_FUNC)
 * @param[in] fmt printf-style format string
 * @param[in] ... printf-style variadic arguments (must match @p fmt)
 */
NEI_API void nei_llog(nei_log_config_handle_t config_handle,
                      nei_log_level_e level,
                      const char *file,
                      int32_t line,
                      const char *func,
                      const char *fmt,
                      ...) PRINTF_LIKE(6, 7);

/**
 * @brief Write a verbose log entry
 *
 * @param[in] config_handle Log configuration handle (typically @ref NEI_LOG_DEFAULT_CONFIG_HANDLE)
 * @param[in] verbose Verbose sub-level (for finer-grained verbose output)
 * @param[in] file Source file path (typically @c __FILE__)
 * @param[in] line Source line number (typically @c __LINE__)
 * @param[in] func Function signature/name (typically @ref NEI_FUNC)
 * @param[in] fmt printf-style format string
 * @param[in] ... printf-style variadic arguments (must match @p fmt)
 */
NEI_API void nei_vlog(
    nei_log_config_handle_t config_handle, int verbose, const char *file, int32_t line, const char *func, const char *fmt, ...)
    PRINTF_LIKE(6, 7);

/**
 * @brief Write a level-based log entry with a pre-formatted literal message
 *
 * @details No format string and no variadic arguments: @p message and @p length are serialized as a
 * single payload. The consumer appends the bytes to the line without printf-style expansion, so you
 * can format elsewhere (e.g. fmt, std::format) and pass the result here.
 *
 * @param[in] message Message bytes (not required to be '\\0'-terminated)
 * @param[in] length Message length in bytes (longer segments are truncated to an internal copy limit)
 */
NEI_API void nei_llog_literal(nei_log_config_handle_t config_handle,
                              nei_log_level_e level,
                              const char *file,
                              int32_t line,
                              const char *func,
                              const char *message,
                              size_t length);

/**
 * @brief Write a verbose log entry with a pre-formatted literal message
 *
 * @copydetails nei_llog_literal
 */
NEI_API void nei_vlog_literal(nei_log_config_handle_t config_handle,
                              int verbose,
                              const char *file,
                              int32_t line,
                              const char *func,
                              const char *message,
                              size_t length);

/**
 * @brief Wait until all asynchronously queued records have been delivered to sinks
 *
 * @details Promotes any partial fill of the active buffer and blocks until the
 * consumer has finished processing all pending data.
 *
 * @warning Do not call from a sink callback (or any code running on the
 * library's consumer thread): @ref nei_log_flush waits until consumption
 * completes while the callback is still part of that consumption, which
 * deadlocks. Calling from other threads is fine subject to your own locking
 * discipline with @ref nei_llog / @ref nei_vlog / @ref nei_llog_literal / @ref nei_vlog_literal.
 */
NEI_API void nei_log_flush(void);

/** @} */ /* end of nei_log_functions */

/**
 * @defgroup nei_log_macros Macros
 * @ingroup nei_log
 * @brief Convenience logging macros.
 * @{
 */

/**
 * @brief Log a TRACE message (convenience macro)
 * @param fmt printf-style format string
 * @param ... printf-style variadic arguments
 */
#define NEI_LOG_TRACE(fmt, ...)                                                                                        \
  nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_LOG_LEVEL_TRACE, __FILE__, __LINE__, NEI_FUNC, fmt, ##__VA_ARGS__)

/**
 * @brief Log a DEBUG message (convenience macro)
 * @param fmt printf-style format string
 * @param ... printf-style variadic arguments
 */
#define NEI_LOG_DEBUG(fmt, ...)                                                                                        \
  nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_LOG_LEVEL_DEBUG, __FILE__, __LINE__, NEI_FUNC, fmt, ##__VA_ARGS__)

/**
 * @brief Log an INFO message (convenience macro)
 * @param fmt printf-style format string
 * @param ... printf-style variadic arguments
 */
#define NEI_LOG_INFO(fmt, ...)                                                                                         \
  nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_LOG_LEVEL_INFO, __FILE__, __LINE__, NEI_FUNC, fmt, ##__VA_ARGS__)

/**
 * @brief Log a WARN message (convenience macro)
 * @param fmt printf-style format string
 * @param ... printf-style variadic arguments
 */
#define NEI_LOG_WARN(fmt, ...)                                                                                         \
  nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_LOG_LEVEL_WARN, __FILE__, __LINE__, NEI_FUNC, fmt, ##__VA_ARGS__)

/**
 * @brief Log an ERROR message (convenience macro)
 * @param fmt printf-style format string
 * @param ... printf-style variadic arguments
 */
#define NEI_LOG_ERROR(fmt, ...)                                                                                        \
  nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_LOG_LEVEL_ERROR, __FILE__, __LINE__, NEI_FUNC, fmt, ##__VA_ARGS__)

/**
 * @brief Log a FATAL message (convenience macro)
 * @param fmt printf-style format string
 * @param ... printf-style variadic arguments
 */
#define NEI_LOG_FATAL(fmt, ...)                                                                                        \
  nei_llog(NEI_LOG_DEFAULT_CONFIG_HANDLE, NEI_LOG_LEVEL_FATAL, __FILE__, __LINE__, NEI_FUNC, fmt, ##__VA_ARGS__)

/**
 * @brief Log a VERBOSE message (convenience macro)
 * @param verbose Verbose sub-level (for finer-grained verbose output)
 * @param fmt printf-style format string
 * @param ... printf-style variadic arguments
 */
#define NEI_LOG_VERBOSE(verbose, fmt, ...)                                                                             \
  nei_vlog(NEI_LOG_DEFAULT_CONFIG_HANDLE, verbose, __FILE__, __LINE__, NEI_FUNC, fmt, ##__VA_ARGS__)

/** @} */ /* end of nei_log_macros */

/** @} */ /* end of nei_log */

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // NEI_LOG_LOG_H
