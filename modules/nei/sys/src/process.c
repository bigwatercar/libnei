#include <nei/sys/process.h>

#include <stdlib.h>
#include <string.h>

/*
 * Encoding notes by platform:
 *
 *   Windows  — GetModuleFileNameW → WideCharToMultiByte(CP_UTF8) → UTF-8
 *   macOS    — _NSGetExecutablePath / realpath → native UTF-8 (NFD)
 *   Linux    — readlink("/proc/self/exe") → raw kernel bytes, typically UTF-8
 */

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else /* Linux / other Unix */
#include <unistd.h>
#endif

int nei_get_executable_path(char *buf, size_t size) {
  if (buf == NULL || size == 0) {
    return -1;
  }

#ifdef _WIN32
  {
    /*
     * GetModuleFileNameW returns UTF-16LE; convert to UTF-8 via CP_UTF8.
     *
     * Use a stack buffer first (MAX_PATH covers the vast majority of cases).
     * If the path is longer than the allocated buffer, the API truncates and
     * returns the buffer size as the length.  We detect truncation by checking
     * whether the returned length equals the buffer capacity, and retry with
     * a dynamically grown heap buffer when necessary.
     */
    wchar_t wbuf_stack[MAX_PATH];
    wchar_t *wbuf = wbuf_stack;
    DWORD wsize = MAX_PATH;
    DWORD len = GetModuleFileNameW(NULL, wbuf, wsize);
    if (len == 0) {
      return -1;
    }

    /* Truncated?  Grow dynamically until it fits. */
    if (len == wsize) {
      wbuf = NULL;
      do {
        if (wsize > 32768U) {
          /* Sanity cap: path longer than 32K wide chars is unreasonable. */
          free(wbuf);
          return -1;
        }
        wsize *= 2U;
        wchar_t *newbuf = (wchar_t *)realloc(wbuf, wsize * sizeof(wchar_t));
        if (newbuf == NULL) {
          free(wbuf);
          return -1;
        }
        wbuf = newbuf;
        len = GetModuleFileNameW(NULL, wbuf, wsize);
      } while (len > 0 && len == wsize);

      if (len == 0) {
        free(wbuf);
        return -1;
      }
    }

    /* Convert the (possibly dynamically allocated) wide string to UTF-8. */
    int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)len, NULL, 0, NULL, NULL);
    if (needed <= 0) {
      if (wbuf != wbuf_stack) free(wbuf);
      return -1;
    }
    if ((size_t)needed >= size) {
      WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)len, buf, (int)(size - 1), NULL, NULL);
      buf[size - 1] = '\0';
      if (wbuf != wbuf_stack) free(wbuf);
      return needed;
    }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)len, buf, needed, NULL, NULL);
    buf[needed] = '\0';
    if (wbuf != wbuf_stack) free(wbuf);
    return needed;
  }
#elif defined(__APPLE__)
  {
    /* _NSGetExecutablePath and realpath return native UTF-8 (NFD) on macOS. */
    uint32_t path_size = (uint32_t)size;
    if (_NSGetExecutablePath(buf, &path_size) != 0) {
      /* Buffer too small: path_size now holds the required size. */
      return (int)path_size;
    }
    /* Resolve symlinks to get the real path. */
    char resolved[4096];
    if (realpath(buf, resolved) != NULL) {
      size_t rlen = strlen(resolved);
      if (rlen >= size) {
        memcpy(buf, resolved, size - 1);
        buf[size - 1] = '\0';
        return (int)rlen;
      }
      memcpy(buf, resolved, rlen + 1);
      return (int)rlen;
    }
    return (int)strlen(buf);
  }
#else
  {
    /* Linux / Unix: readlink returns raw kernel bytes; typically UTF-8
       on modern systems, but could be another 8-bit locale encoding. */
    ssize_t len = readlink("/proc/self/exe", buf, size);
    if (len < 0) {
      return -1;
    }
    if ((size_t)len >= size) {
      /* Truncated: null-terminate what we have and return required size. */
      buf[size - 1] = '\0';
      return (int)len;
    }
    buf[len] = '\0';
    return (int)len;
  }
#endif
}

int nei_get_executable_dir(char *buf, size_t size) {
  if (buf == NULL || size == 0) {
    return -1;
  }

  /* First, get the full executable path (may need an internal buffer). */
  char full_path[4096];
  int full_len = nei_get_executable_path(full_path, sizeof(full_path));
  if (full_len < 0) {
    return full_len;
  }

  /* Find the last path separator (handle both / and \ for cross-platform
     consistency, e.g. WSL interop paths). */
  const char *last_sep = NULL;
  for (int i = full_len - 1; i >= 0; --i) {
    char c = full_path[i];
    if (c == '/' || c == '\\') {
      last_sep = &full_path[i];
      break;
    }
  }

  size_t dir_len;
  const char *src;

  if (last_sep != NULL) {
    dir_len = (size_t)(last_sep - full_path);
    src = full_path;
  } else {
    /* No directory separator found: executable was launched with a bare
       filename.  Return "." to represent the current directory. */
    dir_len = 1;
    src = ".";
  }

  if (dir_len >= size) {
    /* Buffer too small: truncate and return required size. */
    memcpy(buf, src, size - 1);
    buf[size - 1] = '\0';
    return (int)dir_len;
  }

  memcpy(buf, src, dir_len);
  buf[dir_len] = '\0';
  return (int)dir_len;
}
