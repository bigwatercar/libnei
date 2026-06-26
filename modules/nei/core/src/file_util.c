#include <nei/core/file_util.h>
#include <nei/core/encoding.h>

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

FILE *nei_fopen_utf8(const char *path, const char *mode) {
    wchar_t wpath[4096];
    wchar_t wmode[32];
    FILE *fp = NULL;
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) {
        return NULL;
    }
    if (nei_utf8_to_wstr(mode, wmode, 32) < 0) {
        return NULL;
    }
    _wfopen_s(&fp, wpath, wmode);
    return fp;
}

#else /* POSIX */

FILE *nei_fopen_utf8(const char *path, const char *mode) {
    return fopen(path, mode);
}

#endif
