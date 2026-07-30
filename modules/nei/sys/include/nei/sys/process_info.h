#pragma once
#ifndef NEI_SYS_PROCESS_INFO_H
#define NEI_SYS_PROCESS_INFO_H

#include <nei/macros/nei_export.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Process memory information
 * ========================================================================= */

/**
 * @brief Memory usage information for the current process.
 */
typedef struct nei_process_memory_info_st {
  /** Total virtual address space in bytes. */
  uint64_t virtual_bytes;
  /** Physical memory currently in use (working set / RSS) in bytes. */
  uint64_t resident_bytes;
  /** Peak virtual address space ever used, in bytes. */
  uint64_t peak_virtual_bytes;
  /** Peak physical memory ever used, in bytes. */
  uint64_t peak_resident_bytes;
} nei_process_memory_info_st;

/**
 * @brief Get memory usage information for the current process.
 *
 * @param info Output structure (must not be NULL).
 * @return 0 on success, or a negative value on error.
 */
NEI_API int nei_get_process_memory_info(nei_process_memory_info_st *info);

/**
 * @brief Get the current process ID (PID).
 *
 * @return The process ID on success, or a negative value on error.
 */
NEI_API int64_t nei_get_pid(void);

/**
 * @brief Get the parent process ID (PPID).
 *
 * @return The parent process ID on success, or a negative value on error.
 */
NEI_API int64_t nei_get_parent_pid(void);

/**
 * @brief Get the number of milliseconds since the current process started.
 *
 * The uptime is measured from the moment the process was created by the OS
 * (not from the start of @c main).  On Linux, the value is derived from
 * @c /proc/self/stat field 22 (starttime).  On macOS it uses @c sysctl
 * @c KERN_PROC_PID.  On Windows it uses @c GetProcessTimes().
 *
 * @return Uptime in milliseconds on success, or a negative value on error.
 */
NEI_API int64_t nei_get_process_uptime_ms(void);

/**
 * @brief Get the absolute path of the current process's executable file.
 *
 * On Linux, this reads /proc/self/exe.
 * On macOS, this uses _NSGetExecutablePath and resolves symlinks with realpath().
 * On Windows, this uses GetModuleFileNameW.
 *
 * @section encoding Text encoding of the returned path
 *
 * - **Windows**: The path is always returned in **UTF-8** encoding.
 *   The underlying Win32 API produces UTF-16LE; this function converts
 *   it to UTF-8 via WideCharToMultiByte(CP_UTF8).
 *
 * - **macOS**: The path is returned in **UTF-8** encoding.
 *   Both _NSGetExecutablePath and realpath() return UTF-8 on macOS.
 *   macOS filesystem paths are natively decomposed UTF-8 (NFD), so
 *   callers may want to normalize the result if combining forms matter.
 *
 * - **Linux / Unix**: The path is returned in the **native byte encoding**
 *   used by the kernel's filesystem.  On almost all modern Linux systems
 *   this is **UTF-8**, but in theory it could be any 8-bit locale encoding
 *   (e.g. ISO-8859-1, GBK).  The raw bytes from /proc/self/exe are returned
 *   as-is with no conversion.
 *
 * @param buf  Output buffer to receive the path (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value (the
 *         path was truncated and @p buf is null-terminated up to @p size).
 *         On error, returns a negative value.
 */
NEI_API int nei_get_executable_path(char *buf, size_t size);

/**
 * @brief Get the absolute directory path containing the current process's
 *        executable file.
 *
 * This is a convenience wrapper around nei_get_executable_path() that
 * returns only the directory portion of the executable path (i.e., the
 * path with the final filename component removed).
 *
 * If the executable path does not contain any directory separator (which
 * can happen on POSIX when the process was launched with just a filename),
 * the function returns "." (a single dot representing the current directory).
 *
 * The encoding semantics are identical to nei_get_executable_path().
 *
 * @note This function uses an internal 4 KiB (4096-byte) stack buffer to
 *       first obtain the full executable path.  If the full path (including
 *       the filename component) is 4096 bytes or longer, it will be
 *       truncated before the directory split, and the result may be
 *       incomplete.  In practice this is unlikely on all supported
 *       platforms, as filesystem path length limits are typically far
 *       below this threshold.
 *
 * @param buf  Output buffer to receive the directory path (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_executable_dir(char *buf, size_t size);

/**
 * @brief Get the absolute path of the current working directory.
 *
 * On all platforms, the returned string is UTF-8 encoded.
 *
 * @param buf  Output buffer to receive the path (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_current_directory(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <filesystem>
#include <stdexcept>
#include <string>

inline std::string nei_get_current_directory() {
  char buf[4096];
  int len = ::nei_get_current_directory(buf, sizeof(buf));
  if (len < 0) {
    return {};
  }
  return std::string(buf, static_cast<size_t>(len));
}

inline std::filesystem::path nei_get_current_directory_path() {
  char buf[4096];
  int len = ::nei_get_current_directory(buf, sizeof(buf));
  if (len < 0) {
    return {};
  }
#if __cplusplus >= 202002L
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(buf), static_cast<size_t>(len)));
#else
  return std::filesystem::u8path(std::string(buf, static_cast<size_t>(len)));
#endif
}

// These convenience wrappers return std::filesystem::path consistently
// across C++17 and C++20.
//
// The underlying C function returns UTF-8 bytes in a char buffer.
// - C++20: construct std::u8string -> path(char8_t*) interprets UTF-8 natively.
// - C++17: use std::filesystem::u8path() because path(const char*)
//   interprets as the system ANSI/ACP code page on Windows.

inline std::filesystem::path nei_get_executable_path() {
  char buf[4096];
  int len = ::nei_get_executable_path(buf, sizeof(buf));
  if (len < 0) {
    throw std::runtime_error("Failed to get executable path");
  }
#if __cplusplus >= 202002L
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(buf), (size_t)len));
#else
  return std::filesystem::u8path(std::string(buf, (size_t)len));
#endif
}

inline std::filesystem::path nei_get_executable_dir() {
  char buf[4096];
  int len = ::nei_get_executable_dir(buf, sizeof(buf));
  if (len < 0) {
    throw std::runtime_error("Failed to get executable directory");
  }
#if __cplusplus >= 202002L
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(buf), (size_t)len));
#else
  return std::filesystem::u8path(std::string(buf, (size_t)len));
#endif
}

#endif

#endif /* NEI_SYS_PROCESS_INFO_H */
