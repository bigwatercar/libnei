#pragma once
#ifndef NEI_SYS_DISK_INFO_H
#define NEI_SYS_DISK_INFO_H

#include <nei/macros/nei_export.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the total size of the filesystem containing @p path.
 *
 * @param path A path on the filesystem to query. If NULL or empty, the
 *             current working directory is used.
 * @return Total disk space in bytes on success, or 0 on error.
 */
NEI_API uint64_t nei_get_disk_total_space(const char *path);

/**
 * @brief Get the total free space on the filesystem containing @p path.
 *
 * This is the total number of free bytes on the disk, including space
 * reserved for privileged users (on Unix) or not immediately available
 * to the calling user.
 *
 * @param path A path on the filesystem to query. If NULL or empty, the
 *             current working directory is used.
 * @return Free disk space in bytes on success, or 0 on error.
 */
NEI_API uint64_t nei_get_disk_free_space(const char *path);

/**
 * @brief Get the space available to the calling user on the filesystem
 *        containing @p path.
 *
 * On Unix, this may differ from nei_get_disk_free_space() because it
 * excludes blocks reserved for the superuser.  On Windows it is the
 * same as nei_get_disk_free_space() (the value returned by
 * @c GetDiskFreeSpaceExW @c lpFreeBytesAvailableToCaller).
 *
 * @param path A path on the filesystem to query. If NULL or empty, the
 *             current working directory is used.
 * @return Available disk space in bytes on success, or 0 on error.
 */
NEI_API uint64_t nei_get_disk_available_space(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NEI_SYS_DISK_INFO_H */
