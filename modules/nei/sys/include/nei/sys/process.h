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

#ifdef __cplusplus
}
#endif

#endif /* NEI_SYS_PROCESS_H */
