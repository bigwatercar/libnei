#include <nei/sys/disk_info.h>

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

#include <nei/core/encoding.h>

uint64_t nei_get_disk_total_space(const char *path) {
#ifdef _WIN32
  {
    const wchar_t *wpath = NULL;
    wchar_t wpath_buf[MAX_PATH];

    if (path == NULL || path[0] == '\0') {
      /* Use current directory. */
      DWORD wlen = GetCurrentDirectoryW(MAX_PATH, wpath_buf);
      if (wlen == 0 || wlen > MAX_PATH) {
        return 0;
      }
      wpath = wpath_buf;
    } else {
      if (nei_utf8_to_wstr(path, wpath_buf, MAX_PATH) < 0) {
        return 0;
      }
      wpath = wpath_buf;
    }

    ULARGE_INTEGER total, free, avail;
    if (!GetDiskFreeSpaceExW(wpath, &avail, &total, &free)) {
      return 0;
    }
    return (uint64_t)total.QuadPart;
  }
#else
  {
    const char *p = (path != NULL && path[0] != '\0') ? path : ".";
    struct statvfs st;
    if (statvfs(p, &st) != 0) {
      return 0;
    }
    return (uint64_t)st.f_blocks * (uint64_t)st.f_frsize;
  }
#endif
}

uint64_t nei_get_disk_free_space(const char *path) {
#ifdef _WIN32
  {
    const wchar_t *wpath = NULL;
    wchar_t wpath_buf[MAX_PATH];

    if (path == NULL || path[0] == '\0') {
      DWORD wlen = GetCurrentDirectoryW(MAX_PATH, wpath_buf);
      if (wlen == 0 || wlen > MAX_PATH) {
        return 0;
      }
      wpath = wpath_buf;
    } else {
      if (nei_utf8_to_wstr(path, wpath_buf, MAX_PATH) < 0) {
        return 0;
      }
      wpath = wpath_buf;
    }

    ULARGE_INTEGER total, free, avail;
    if (!GetDiskFreeSpaceExW(wpath, &avail, &total, &free)) {
      return 0;
    }
    /* lpTotalNumberOfFreeBytes = free (includes space reserved for quota) */
    return (uint64_t)free.QuadPart;
  }
#else
  {
    const char *p = (path != NULL && path[0] != '\0') ? path : ".";
    struct statvfs st;
    if (statvfs(p, &st) != 0) {
      return 0;
    }
    return (uint64_t)st.f_bfree * (uint64_t)st.f_frsize;
  }
#endif
}

uint64_t nei_get_disk_available_space(const char *path) {
#ifdef _WIN32
  {
    const wchar_t *wpath = NULL;
    wchar_t wpath_buf[MAX_PATH];

    if (path == NULL || path[0] == '\0') {
      DWORD wlen = GetCurrentDirectoryW(MAX_PATH, wpath_buf);
      if (wlen == 0 || wlen > MAX_PATH) {
        return 0;
      }
      wpath = wpath_buf;
    } else {
      if (nei_utf8_to_wstr(path, wpath_buf, MAX_PATH) < 0) {
        return 0;
      }
      wpath = wpath_buf;
    }

    ULARGE_INTEGER total, free, avail;
    if (!GetDiskFreeSpaceExW(wpath, &avail, &total, &free)) {
      return 0;
    }
    return (uint64_t)avail.QuadPart;
  }
#else
  {
    const char *p = (path != NULL && path[0] != '\0') ? path : ".";
    struct statvfs st;
    if (statvfs(p, &st) != 0) {
      return 0;
    }
    return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
  }
#endif
}
