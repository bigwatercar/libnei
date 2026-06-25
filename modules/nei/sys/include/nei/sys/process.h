#pragma once
#ifndef NEI_SYS_PROCESS_H
#define NEI_SYS_PROCESS_H

#include <nei/macros/nei_export.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <string>
#include <stdexcept>

// When compiling with C++20 or later, these wrappers return std::u8string.
// When compiling with C++17 or earlier, they return std::string, but the
// string **always** contains UTF-8 encoded data regardless of the return type.

#if __cplusplus >= 202002L

inline std::u8string nei_get_executable_path() {
  char buf[4096];
  int len = nei_get_executable_path(buf, sizeof(buf));
  if (len < 0) {
    throw std::runtime_error("Failed to get executable path");
  }
  return std::u8string(reinterpret_cast<const char8_t *>(buf), (size_t)len);
}

inline std::u8string nei_get_executable_dir() {
  char buf[4096];
  int len = nei_get_executable_dir(buf, sizeof(buf));
  if (len < 0) {
    throw std::runtime_error("Failed to get executable directory");
  }
  return std::u8string(reinterpret_cast<const char8_t *>(buf), (size_t)len);
}

#else // __cplusplus < 202002L

// NOTE: The returned std::string stores UTF-8 encoded bytes.
// Callers should treat the content as UTF-8.
inline std::string nei_get_executable_path() {
  char buf[4096];
  int len = nei_get_executable_path(buf, sizeof(buf));
  if (len < 0) {
    throw std::runtime_error("Failed to get executable path");
  }
  return std::string(buf, (size_t)len);
}

// NOTE: The returned std::string stores UTF-8 encoded bytes.
// Callers should treat the content as UTF-8.
inline std::string nei_get_executable_dir() {
  char buf[4096];
  int len = nei_get_executable_dir(buf, sizeof(buf));
  if (len < 0) {
    throw std::runtime_error("Failed to get executable directory");
  }
  return std::string(buf, (size_t)len);
}

#endif // __cplusplus >= 202002L

#endif

#endif /* NEI_SYS_PROCESS_H */
