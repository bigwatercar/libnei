#pragma once
#ifndef NEI_SYS_HOST_INFO_H
#define NEI_SYS_HOST_INFO_H

#include <nei/macros/nei_export.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the hostname of the current machine.
 *
 * On all platforms, the returned string is UTF-8 encoded.
 *
 * @param buf  Output buffer to receive the hostname (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_hostname(char *buf, size_t size);

/**
 * @brief Get the login name of the current user.
 *
 * On all platforms, the returned string is UTF-8 encoded.
 *
 * @param buf  Output buffer to receive the username (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_username(char *buf, size_t size);

/**
 * @brief Get the absolute path to the current user's home directory.
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
NEI_API int nei_get_home_dir(char *buf, size_t size);

/**
 * @brief Get the absolute path to the system temporary directory.
 *
 * On all platforms, the returned string is UTF-8 encoded. The returned
 * path includes a trailing directory separator.
 *
 * @param buf  Output buffer to receive the path (must not be NULL).
 * @param size Size of @p buf in bytes.
 * @return On success, returns the number of bytes written to @p buf
 *         (excluding the null terminator). If @p buf is too small,
 *         returns the required buffer size as a positive value.
 *         On error, returns a negative value.
 */
NEI_API int nei_get_temp_dir(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <filesystem>
#include <string>

inline std::string nei_get_hostname() {
    char buf[256];
    int len = ::nei_get_hostname(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(len));
}

inline std::string nei_get_username() {
    char buf[256];
    int len = ::nei_get_username(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(len));
}

inline std::string nei_get_home_dir() {
    char buf[4096];
    int len = ::nei_get_home_dir(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(len));
}

inline std::filesystem::path nei_get_home_dir_path() {
    char buf[4096];
    int len = ::nei_get_home_dir(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
#if __cplusplus >= 202002L
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(buf), static_cast<size_t>(len)));
#else
    return std::filesystem::u8path(std::string(buf, static_cast<size_t>(len)));
#endif
}

inline std::string nei_get_temp_dir() {
    char buf[4096];
    int len = ::nei_get_temp_dir(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
    return std::string(buf, static_cast<size_t>(len));
}

inline std::filesystem::path nei_get_temp_dir_path() {
    char buf[4096];
    int len = ::nei_get_temp_dir(buf, sizeof(buf));
    if (len < 0) {
        return {};
    }
#if __cplusplus >= 202002L
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(buf), static_cast<size_t>(len)));
#else
    return std::filesystem::u8path(std::string(buf, static_cast<size_t>(len)));
#endif
}

#endif

#endif /* NEI_SYS_HOST_INFO_H */
