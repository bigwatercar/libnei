#pragma once
#ifndef NEI_BUILD_NEI_GLOBAL_H
#define NEI_BUILD_NEI_GLOBAL_H

#include <stdint.h>

#include <nei/build/nei_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 库自身诊断日志通道（libnei 内部 NEI_LOG_C 的目标 config 句柄）。
 *
 * 类型为底层整数 uintptr_t，与 nei/log/log.h 中 nei_log_config_handle_t
 * 的底层类型一致——直接用底层类型是为了避免本头文件暴露 log.h。
 *
 * 默认值为无效句柄（0），即库诊断默认静默——库不会擅自向客户端的
 * stdout/stderr 输出。客户端可选择：
 *  - 直接赋值为自己创建的 config 句柄（复用客户端日志通道）；或
 *  - 调用 @ref nei_enable_diagnostic_message_to_stdout 创建独立 stdout 通道。
 *
 * 约定：在进程启动期设置/卸载，运行期由内部日志调用只读（非原子）。
 */
NEI_API extern uintptr_t g_nei_logger;

/**
 * @brief 开启库诊断输出到 stdout。
 *
 * @details 若 @ref g_nei_logger 已有效则 no-op（幂等）；否则创建独立 config
 * （仅注册一个 stdout sink，不借用默认 config 的 sinks）并赋值给
 * g_nei_logger。sink 所有权移交 config，由 @ref nei_shutdown_diagnostic_message
 * 统一释放。线程不安全，应在进程启动期调用。
 */
NEI_API void nei_enable_diagnostic_message_to_stdout(void);

/**
 * @brief 卸载库诊断通道。
 *
 * @details 移除 g_nei_logger 指向的 config（内部双 flush 后释放全部
 * sinks），并把 g_nei_logger 重置为无效。对无效句柄 no-op。
 * 线程不安全，应在进程退出期调用。
 */
NEI_API void nei_shutdown_diagnostic_message(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // NEI_BUILD_NEI_GLOBAL_H
