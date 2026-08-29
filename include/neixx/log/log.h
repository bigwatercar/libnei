/**
 * @file log.h
 * @author ylf
 * @brief NEIXX C++ Log Macros  --  C++ convenience wrapper over nei/log/log.h
 * @version 0.1
 * @date 2026-06-22
 *
 * @copyright Copyright (c) 2026
 *
 * @details
 * This header provides C++ convenience macros that format messages with
 * {fmt} or C++20 `std::format` and emit them through the C logging API
 * (`nei_llog_literal` / `nei_vlog_literal`).  It is a C++ extension of
 * @ref nei/log/log.h and respects the same @c NEI_LOG_DISABLE_MACROS guard.
 *
 * @par Format backend selection
 * | Condition                              | Backend           |
 * |----------------------------------------|-------------------|
 * | `__has_include(<fmt/format.h>)`         | `fmt::format`     |
 * | C++20 and `__has_include(<format>)`     | `std::format`     |
 * | Otherwise                              | Compile-time error |
 *
 * @par Usage
 * @code
 * #include <neixx/log/log.h>
 *
 * NEIXX_LOG(NEI_L_INFO, "Hello {}", "world");
 * NEIXX_LOG_IF(x > 0, NEI_L_WARN, "x = {}", x);
 * NEIXX_LOG_V(2, "Verbose detail: {}", detail);
 * NEIXX_LOG_V_IF(debug_mode, 1, "Debug: {}", value);
 * @endcode
 */

#pragma once
#ifndef NEIXX_LOG_LOG_H
#define NEIXX_LOG_LOG_H

#include <nei/log/log.h>

// ── Format backend selection ─────────────────────────────────────────────

#if __has_include(<fmt/format.h>)
#include <fmt/format.h>
#define NEIXX_LOG_FMT(fmt_str, ...) fmt::format(fmt_str, ##__VA_ARGS__)
#elif __cplusplus >= 202002L && __has_include(<format>)
#include <format>
#define NEIXX_LOG_FMT(fmt_str, ...) std::format(fmt_str, ##__VA_ARGS__)
#else
#error "neixx/log/log.h requires either {fmt} (<fmt/format.h>) or C++20 <format>"
#endif

// ── Macros ────────────────────────────────────────────────────────────────

#if !defined(NEI_LOG_DISABLE_MACROS)

/**
 * @brief Log a level-based message with C++ format string (convenience macro)
 * @param level @ref nei_log_level_e value
 * @param fmt_str Format string (fmt / std::format syntax)
 * @param ... Format arguments
 */
#define NEIXX_LOG(level, fmt_str, ...)                                                                                 \
  do {                                                                                                                 \
    auto _neixx_msg_ = NEIXX_LOG_FMT(fmt_str, ##__VA_ARGS__);                                                          \
    nei_llog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE,                                                                    \
                     level,                                                                                            \
                     __FILE__,                                                                                         \
                     __LINE__,                                                                                         \
                     NEI_FUNC,                                                                                         \
                     (_neixx_msg_).data(),                                                                             \
                     (_neixx_msg_).size());                                                                            \
  } while (0)

/**
 * @brief Conditionally log a level-based message with C++ format string
 * @param condition Expression evaluated once; log is emitted when non-zero
 * @param level @ref nei_log_level_e value
 * @param fmt_str Format string (fmt / std::format syntax)
 * @param ... Format arguments
 */
#define NEIXX_LOG_IF(condition, level, fmt_str, ...)                                                                   \
  do {                                                                                                                 \
    if (condition) {                                                                                                   \
      NEIXX_LOG(level, fmt_str, ##__VA_ARGS__);                                                                        \
    }                                                                                                                  \
  } while (0)

/**
 * @brief Log a level-based message with C++ format string to a specific config
 * @param config_handle @ref nei_log_config_handle_t target config handle
 * @param level @ref nei_log_level_e value
 * @param fmt_str Format string (fmt / std::format syntax)
 * @param ... Format arguments
 */
#define NEIXX_LOG_C(config_handle, level, fmt_str, ...)                                                                \
  do {                                                                                                                 \
    auto _neixx_msg_ = NEIXX_LOG_FMT(fmt_str, ##__VA_ARGS__);                                                          \
    nei_llog_literal(config_handle, level, __FILE__, __LINE__, NEI_FUNC, (_neixx_msg_).data(), (_neixx_msg_).size());  \
  } while (0)

/**
 * @brief Conditionally log a level-based message with C++ format string to a specific config
 * @param condition Expression evaluated once; log is emitted when non-zero
 * @param config_handle @ref nei_log_config_handle_t target config handle
 * @param level @ref nei_log_level_e value
 * @param fmt_str Format string (fmt / std::format syntax)
 * @param ... Format arguments
 */
#define NEIXX_LOG_C_IF(condition, config_handle, level, fmt_str, ...)                                                  \
  do {                                                                                                                 \
    if (condition) {                                                                                                   \
      NEIXX_LOG_C(config_handle, level, fmt_str, ##__VA_ARGS__);                                                       \
    }                                                                                                                  \
  } while (0)

/**
 * @brief Log a verbose message with C++ format string (convenience macro)
 * @param verbose Verbose sub-level
 * @param fmt_str Format string (fmt / std::format syntax)
 * @param ... Format arguments
 */
#define NEIXX_LOG_V(verbose, fmt_str, ...)                                                                             \
  do {                                                                                                                 \
    auto _neixx_msg_ = NEIXX_LOG_FMT(fmt_str, ##__VA_ARGS__);                                                          \
    nei_vlog_literal(NEI_LOG_DEFAULT_CONFIG_HANDLE,                                                                    \
                     verbose,                                                                                          \
                     __FILE__,                                                                                         \
                     __LINE__,                                                                                         \
                     NEI_FUNC,                                                                                         \
                     (_neixx_msg_).data(),                                                                             \
                     (_neixx_msg_).size());                                                                            \
  } while (0)

/**
 * @brief Conditionally log a verbose message with C++ format string
 * @param condition Expression evaluated once; log is emitted when non-zero
 * @param verbose Verbose sub-level
 * @param fmt_str Format string (fmt / std::format syntax)
 * @param ... Format arguments
 */
#define NEIXX_LOG_V_IF(condition, verbose, fmt_str, ...)                                                               \
  do {                                                                                                                 \
    if (condition) {                                                                                                   \
      NEIXX_LOG_V(verbose, fmt_str, ##__VA_ARGS__);                                                                    \
    }                                                                                                                  \
  } while (0)

/**
 * @brief Log a verbose message with C++ format string to a specific config
 * @param config_handle @ref nei_log_config_handle_t target config handle
 * @param verbose Verbose sub-level
 * @param fmt_str Format string (fmt / std::format syntax)
 * @param ... Format arguments
 */
#define NEIXX_LOG_V_C(config_handle, verbose, fmt_str, ...)                                                            \
  do {                                                                                                                 \
    auto _neixx_msg_ = NEIXX_LOG_FMT(fmt_str, ##__VA_ARGS__);                                                          \
    nei_vlog_literal(                                                                                                  \
        config_handle, verbose, __FILE__, __LINE__, NEI_FUNC, (_neixx_msg_).data(), (_neixx_msg_).size());             \
  } while (0)

/**
 * @brief Conditionally log a verbose message with C++ format string to a specific config
 * @param condition Expression evaluated once; log is emitted when non-zero
 * @param config_handle @ref nei_log_config_handle_t target config handle
 * @param verbose Verbose sub-level
 * @param fmt_str Format string (fmt / std::format syntax)
 * @param ... Format arguments
 */
#define NEIXX_LOG_V_C_IF(condition, config_handle, verbose, fmt_str, ...)                                              \
  do {                                                                                                                 \
    if (condition) {                                                                                                   \
      NEIXX_LOG_V_C(config_handle, verbose, fmt_str, ##__VA_ARGS__);                                                   \
    }                                                                                                                  \
  } while (0)

#else // NEI_LOG_DISABLE_MACROS

#define NEIXX_LOG(level, fmt_str, ...) ((void)(level))
#define NEIXX_LOG_IF(condition, level, fmt_str, ...) ((void)(condition))
#define NEIXX_LOG_C(config_handle, level, fmt_str, ...) ((void)(config_handle), (void)(level))
#define NEIXX_LOG_C_IF(condition, config_handle, level, fmt_str, ...)                                                  \
  ((void)(condition), (void)(config_handle), (void)(level))
#define NEIXX_LOG_V(verbose, fmt_str, ...) ((void)(verbose))
#define NEIXX_LOG_V_IF(condition, verbose, fmt_str, ...) ((void)(condition))
#define NEIXX_LOG_V_C(config_handle, verbose, fmt_str, ...) ((void)(config_handle), (void)(verbose))
#define NEIXX_LOG_V_C_IF(condition, config_handle, verbose, fmt_str, ...)                                              \
  ((void)(condition), (void)(config_handle), (void)(verbose))

#endif // NEI_LOG_DISABLE_MACROS

#endif // NEIXX_LOG_LOG_H
