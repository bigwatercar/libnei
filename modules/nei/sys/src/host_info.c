#include <nei/sys/host_info.h>

#include <nei/core/encoding.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#endif

int nei_get_hostname(char *buf, size_t size) {
  if (buf == NULL || size == 0) {
    return -1;
  }

#ifdef _WIN32
  {
    wchar_t wbuf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD wlen = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(wbuf, &wlen)) {
      return -1;
    }
    return nei_wstr_to_utf8(wbuf, (int)wlen, buf, size);
  }
#else
  {
    if (gethostname(buf, size) != 0) {
      /* gethostname may not null-terminate on ENAMETOOLONG. */
      buf[size - 1] = '\0';
      return -1;
    }
    /*
     * gethostname does not guarantee null termination if the hostname
     * is longer than the buffer. Ensure it.
     */
    buf[size - 1] = '\0';
    return (int)strlen(buf);
  }
#endif
}

int nei_get_username(char *buf, size_t size) {
  if (buf == NULL || size == 0) {
    return -1;
  }

#ifdef _WIN32
  {
    wchar_t wbuf[256];
    DWORD wlen = 256;
    if (!GetUserNameW(wbuf, &wlen)) {
      return -1;
    }
    return nei_wstr_to_utf8(wbuf, (int)wlen, buf, size);
  }
#else
  {
    const char *name = getenv("USER");
    if (name == NULL) {
      name = getenv("LOGNAME");
    }
    if (name == NULL) {
      struct passwd *pw = getpwuid(getuid());
      if (pw == NULL) {
        return -1;
      }
      name = pw->pw_name;
    }
    size_t len = strlen(name);
    if (len >= size) {
      memcpy(buf, name, size - 1);
      buf[size - 1] = '\0';
      return (int)len;
    }
    memcpy(buf, name, len + 1);
    return (int)len;
  }
#endif
}

int nei_get_home_dir(char *buf, size_t size) {
  if (buf == NULL || size == 0) {
    return -1;
  }

#ifdef _WIN32
  {
    /*
     * Use SHGetFolderPathW with CSIDL_PROFILE for the user's home
     * directory, then convert to UTF-8.
     */
    wchar_t wbuf[MAX_PATH];
    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, wbuf);
    if (FAILED(hr)) {
      /* Fallback: try USERPROFILE environment variable. */
      DWORD wlen = GetEnvironmentVariableW(L"USERPROFILE", wbuf, MAX_PATH);
      if (wlen == 0 || wlen >= MAX_PATH) {
        /* Last resort: HOMEDRIVE + HOMEPATH */
        wchar_t drive[32], wpath[MAX_PATH];
        DWORD dlen = GetEnvironmentVariableW(L"HOMEDRIVE", drive, 32);
        DWORD plen = GetEnvironmentVariableW(L"HOMEPATH", wpath, MAX_PATH);
        if (dlen > 0 && dlen < 32 && plen > 0 && plen < MAX_PATH) {
          wcscpy_s(wbuf, MAX_PATH, drive);
          wcscat_s(wbuf, MAX_PATH, wpath);
        } else {
          return -1;
        }
      }
    }

    return nei_wstr_to_utf8(wbuf, -1, buf, size);
  }
#else
  {
    const char *home = getenv("HOME");
    if (home == NULL) {
      struct passwd *pw = getpwuid(getuid());
      if (pw == NULL) {
        return -1;
      }
      home = pw->pw_dir;
    }
    size_t len = strlen(home);
    if (len >= size) {
      memcpy(buf, home, size - 1);
      buf[size - 1] = '\0';
      return (int)len;
    }
    memcpy(buf, home, len + 1);
    return (int)len;
  }
#endif
}

int nei_get_temp_dir(char *buf, size_t size) {
  if (buf == NULL || size == 0) {
    return -1;
  }

#ifdef _WIN32
  {
    wchar_t wbuf[MAX_PATH + 1];
    DWORD wlen = GetTempPathW(MAX_PATH + 1, wbuf);
    if (wlen == 0 || wlen > MAX_PATH) {
      return -1;
    }
    /* GetTempPathW includes a trailing backslash; strip it. */
    if (wlen > 0 && wbuf[wlen - 1] == L'\\') {
      wbuf[wlen - 1] = L'\0';
      --wlen;
    }
    return nei_wstr_to_utf8(wbuf, (int)wlen, buf, size);
  }
#else
  {
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL) {
      tmp = "/tmp";
    }
    size_t len = strlen(tmp);
    /* Strip trailing slash if present (from TMPDIR env). */
    if (len > 1 && tmp[len - 1] == '/') {
      --len;
    }
    if (len >= size) {
      memcpy(buf, tmp, size - 1);
      buf[size - 1] = '\0';
      return (int)len;
    }
    memcpy(buf, tmp, len);
    buf[len] = '\0';
    return (int)len;
  }
#endif
}
