#include <nei/sys/process_info.h>

#include <nei/core/encoding.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#else /* Linux / other Unix */
#include <stdio.h>
#include <unistd.h>
#endif

int64_t nei_get_pid(void) {
#ifdef _WIN32
  return (int64_t)GetCurrentProcessId();
#else
  return (int64_t)getpid();
#endif
}

int64_t nei_get_parent_pid(void) {
#ifdef _WIN32
  /*
   * Windows does not have a direct GetParentProcessId API until
   * Windows Vista+ (kernel32.dll).  Use NtQueryInformationProcess
   * or the Toolhelp32 snapshot as a reliable cross-version method.
   */
  DWORD pid = GetCurrentProcessId();
  DWORD ppid = 0;

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return -1;
  }

  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snapshot, &pe)) {
    do {
      if (pe.th32ProcessID == pid) {
        ppid = pe.th32ParentProcessID;
        break;
      }
    } while (Process32NextW(snapshot, &pe));
  }

  CloseHandle(snapshot);

  if (ppid == 0) {
    return -1;
  }
  return (int64_t)ppid;
#else
  return (int64_t)getppid();
#endif
}

int64_t nei_get_process_uptime_ms(void) {
#ifdef _WIN32
  {
    /*
     * Get process creation time via GetProcessTimes, then compute
     * the difference from the current time.
     */
    HANDLE hproc = GetCurrentProcess();
    FILETIME ft_create, ft_exit, ft_kernel, ft_user;
    if (!GetProcessTimes(hproc, &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
      return -1;
    }

    FILETIME ft_now;
    GetSystemTimeAsFileTime(&ft_now);

    ULARGE_INTEGER ul_create, ul_now;
    ul_create.LowPart = ft_create.dwLowDateTime;
    ul_create.HighPart = ft_create.dwHighDateTime;
    ul_now.LowPart = ft_now.dwLowDateTime;
    ul_now.HighPart = ft_now.dwHighDateTime;

    /*
     * FILETIME is in 100-nanosecond intervals.
     * Convert to milliseconds.
     */
    uint64_t diff = ul_now.QuadPart - ul_create.QuadPart;
    return (int64_t)(diff / 10000ULL);
  }
#elif defined(__APPLE__)
  {
    /*
     * Use sysctl KERN_PROC_PID to get process start time.
     */
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0) {
      return -1;
    }
    if (len == 0) {
      return -1;
    }

    /* kp.kp_proc.p_starttime is a struct timeval (seconds + microseconds). */
    struct timeval now;
    gettimeofday(&now, NULL);

    int64_t start_ms = (int64_t)kp.kp_proc.p_starttime.tv_sec * 1000LL + kp.kp_proc.p_starttime.tv_usec / 1000;
    int64_t now_ms = (int64_t)now.tv_sec * 1000LL + now.tv_usec / 1000;

    return now_ms - start_ms;
  }
#else
  {
    /*
     * On Linux, read /proc/self/stat field 22 (starttime).
     * starttime is in clock ticks since boot; we need to convert
     * to wall-clock milliseconds.
     *
     * Format: pid (comm) state ... starttime ...
     * Field 22 (1-indexed) is starttime.
     */
    FILE *f = fopen("/proc/self/stat", "r");
    if (f == NULL) {
      return -1;
    }

    /* Skip pid and comm (which may contain spaces and parens). */
    unsigned long long starttime = 0;
    char comm[256];
    char state;
    int ppid, pgrp, session, tty_nr, tpgid;
    unsigned int flags;
    unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
    long cutime, cstime, priority, nice, num_threads, itrealvalue;

    int matched = fscanf(f,
                         "%*d %255s %c %d %d %d %d %d "
                         "%u %lu %lu %lu %lu %lu %lu "
                         "%ld %ld %ld %ld %ld %ld %llu",
                         comm,
                         &state,
                         &ppid,
                         &pgrp,
                         &session,
                         &tty_nr,
                         &tpgid,
                         &flags,
                         &minflt,
                         &cminflt,
                         &majflt,
                         &cmajflt,
                         &utime,
                         &stime,
                         &cutime,
                         &cstime,
                         &priority,
                         &nice,
                         &num_threads,
                         &itrealvalue,
                         &starttime);
    fclose(f);

    if (matched < 21) {
      return -1;
    }

    /*
     * Convert starttime (clock ticks since boot) to ms since boot,
     * then compute wall-clock uptime.
     */
    long ticks_per_sec = sysconf(_SC_CLK_TCK);
    if (ticks_per_sec <= 0) {
      ticks_per_sec = 100; /* Fallback */
    }

    int64_t start_ms_since_boot = (int64_t)starttime * 1000LL / (int64_t)ticks_per_sec;

    /*
     * Now get system uptime from /proc/uptime to compute
     * wall-clock process uptime.
     */
    FILE *uf = fopen("/proc/uptime", "r");
    if (uf == NULL) {
      return -1;
    }
    double sys_uptime_sec = 0.0;
    if (fscanf(uf, "%lf", &sys_uptime_sec) != 1) {
      fclose(uf);
      return -1;
    }
    fclose(uf);

    int64_t sys_uptime_ms = (int64_t)(sys_uptime_sec * 1000.0);
    return sys_uptime_ms - start_ms_since_boot;
  }
#endif
}

int nei_get_current_directory(char *buf, size_t size) {
  if (buf == NULL || size == 0) {
    return -1;
  }

#ifdef _WIN32
  {
    wchar_t wbuf[MAX_PATH];
    DWORD wlen = GetCurrentDirectoryW(MAX_PATH, wbuf);
    if (wlen == 0 || wlen > MAX_PATH) {
      return -1;
    }
    return nei_wstr_to_utf8(wbuf, (int)wlen, buf, size);
  }
#else
  {
    if (getcwd(buf, size) == NULL) {
      return -1;
    }
    return (int)strlen(buf);
  }
#endif
}

/*
 * Encoding notes by platform:
 *
 *   Windows   --  GetModuleFileNameW -> WideCharToMultiByte(CP_UTF8) -> UTF-8
 *   macOS     --  _NSGetExecutablePath / realpath -> native UTF-8 (NFD)
 *   Linux     --  readlink("/proc/self/exe") -> raw kernel bytes, typically UTF-8
 */

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
    int result = nei_wstr_to_utf8(wbuf, (int)len, buf, size);
    if (wbuf != wbuf_stack)
      free(wbuf);
    return result;
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

/* =========================================================================
 * Process memory information
 * ========================================================================= */

int nei_get_process_memory_info(nei_process_memory_info_st *info) {
  if (info == NULL) {
    return -1;
  }

#ifdef _WIN32
  {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
      return -1;
    }
    info->virtual_bytes = pmc.PrivateUsage;
    info->resident_bytes = pmc.WorkingSetSize;
    info->peak_virtual_bytes = pmc.PeakPagefileUsage;
    info->peak_resident_bytes = pmc.PeakWorkingSetSize;
    return 0;
  }
#elif defined(__APPLE__)
  {
    /* Resident + peak resident via task_info. */
    struct task_basic_info_64 tbi;
    mach_msg_type_number_t count = TASK_BASIC_INFO_64_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO_64, (task_info_t)&tbi, &count) != KERN_SUCCESS) {
      return -1;
    }
    info->resident_bytes = tbi.resident_size;
    info->peak_resident_bytes = tbi.resident_size_max;
    info->virtual_bytes = tbi.virtual_size;

    /* Peak virtual size is not directly available on macOS.
     * Use virtual_size as an approximation. */
    info->peak_virtual_bytes = tbi.virtual_size;
    return 0;
  }
#else
  {
    /* Linux: parse /proc/self/status. */
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL) {
      return -1;
    }

    memset(info, 0, sizeof(*info));
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
      uint64_t kb = 0;
      if (sscanf(line, "VmPeak: %lu kB", &kb) == 1) {
        info->peak_virtual_bytes = kb * 1024ULL;
      } else if (sscanf(line, "VmSize: %lu kB", &kb) == 1) {
        info->virtual_bytes = kb * 1024ULL;
      } else if (sscanf(line, "VmHWM: %lu kB", &kb) == 1) {
        info->peak_resident_bytes = kb * 1024ULL;
      } else if (sscanf(line, "VmRSS: %lu kB", &kb) == 1) {
        info->resident_bytes = kb * 1024ULL;
      }
    }
    fclose(f);
    return 0;
  }
#endif
}
