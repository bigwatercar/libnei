#include <nei/core/file_util.h>
#include <nei/core/encoding.h>

#include <stdio.h>
#include <sys/stat.h>
#include <io.h>
#include <windows.h>

FILE *nei_fopen_utf8(const char *path, const char *mode) {
    wchar_t wpath[4096];
    wchar_t wmode[32];
    FILE *fp = NULL;
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return NULL;
    if (nei_utf8_to_wstr(mode, wmode, 32)  < 0) return NULL;
    _wfopen_s(&fp, wpath, wmode);
    return fp;
}

int nei_file_size(const char *path, uint64_t *out_size) {
    wchar_t wpath[4096];
    struct _stat64 st;
    if (out_size == NULL)                         return -1;
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return -1;
    if (_wstat64(wpath, &st) != 0)                return -1;
    if ((st.st_mode & _S_IFREG) == 0)             return -1;
    *out_size = (uint64_t)st.st_size;
    return 0;
}

int nei_file_remove(const char *path) {
    wchar_t wpath[4096];
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return -1;
    return _wremove(wpath) == 0 ? 0 : -1;
}

int nei_file_rename(const char *old_path, const char *new_path) {
    wchar_t wold[4096], wnew[4096];
    if (nei_utf8_to_wstr(old_path, wold, 4096) < 0) return -1;
    if (nei_utf8_to_wstr(new_path, wnew, 4096) < 0) return -1;
    return _wrename(wold, wnew) == 0 ? 0 : -1;
}
