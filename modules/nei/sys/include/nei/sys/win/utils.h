#pragma once
#ifndef NEI_SYS_WIN_UTILS_H
#define NEI_SYS_WIN_UTILS_H

/*
 * Windows-specific system utilities.
 *
 * These functions are only available on Windows.  On other platforms
 * including this header will produce a compile-time error.
 */

#ifdef _WIN32

#include <nei/build/nei_export.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve a @c .lnk shortcut file to its target path.
 *
 * Uses the COM @c IShellLinkW interface to read the shortcut's target.
 *
 * @param lnk_path UTF-8 path to the shortcut file (must not be NULL).
 * @param buf      Output buffer for the resolved target path (must not be NULL).
 * @param size     Size of @p buf in bytes.
 * @return On success, the number of UTF-8 bytes written (excluding the
 *         null terminator).  If @p buf is too small, the content is
 *         truncated, null-terminated, and the required buffer size is
 *         returned as a positive value.  On error, returns a negative
 *         value (e.g. file not found, not a shortcut, COM failure).
 *
 * @note The returned path may still be a shortcut  --  some shortcuts chain
 *       to other shortcuts.  Callers should check and resolve iteratively
 *       if needed.
 */
NEI_API int nei_win_resolve_shortcut(const char *lnk_path, char *buf, size_t size);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------------------------------------------
 * C++ convenience wrapper
 * --------------------------------------------------------------------------- */
#ifdef __cplusplus

#include <filesystem>
#include <string>

/**
 * @brief Resolve a @c .lnk shortcut and return the target as a path.
 *
 * @param lnk_path UTF-8 path to the shortcut.
 * @return The resolved target path, or an empty path on failure.
 */
inline std::filesystem::path nei_win_resolve_shortcut(const char *lnk_path) {
  char buf[4096];
  int len = ::nei_win_resolve_shortcut(lnk_path, buf, sizeof(buf));
  if (len < 0)
    return {};
#if __cplusplus >= 202002L
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(buf), static_cast<size_t>(len)));
#else
  return std::filesystem::u8path(std::string(buf, static_cast<size_t>(len)));
#endif
}

#endif /* __cplusplus */
#endif /* _WIN32 */
#endif /* NEI_SYS_WIN_UTILS_H */
