#include <nei/sys/fs_util.h>
#include <nei/core/encoding.h>

#include <windows.h>

int nei_is_file_busy(const char *path) {
  HANDLE h;
  DWORD err;
  wchar_t wpath[4096];

  if (path == NULL)
    return -1;

  /* Convert UTF-8 path to wide. */
  if (nei_utf8_to_wstr(path, wpath, 4096) < 0)
    return -1;

  /* Attempt to open the file with no access and no sharing allowed.
   * If ANY other handle exists on this file (regardless of how
   * permissive its sharing mode is), this will fail with a sharing
   * violation.  dwDesiredAccess = 0 means we don't need to read or
   * write  --  we just want to test exclusivity. */
  h = CreateFileW(wpath,
                  0, /* no access needed */
                  0, /* no sharing allowed */
                  NULL,
                  OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL,
                  NULL);

  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
    return 0; /* file is free */
  }

  err = GetLastError();
  if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
    return 1; /* file is busy */
  }

  /* ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, etc. */
  return -1;
}
