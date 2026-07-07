#include <nei/core/path_util.h>
#include <nei/core/encoding.h>

#include <string.h>
#include <sys/stat.h>
#include <io.h>
#include <windows.h>
#include <direct.h>
#include <wchar.h>

int nei_path_exists(const char *path) {
    if (path == NULL) return 0;
    wchar_t wpath[4096];
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return 0;
    return _waccess(wpath, 0) == 0;
}

int nei_path_is_file(const char *path) {
    if (path == NULL) return 0;
    wchar_t wpath[4096];
    struct _stat64 st;
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return 0;
    if (_wstat64(wpath, &st) != 0) return 0;
    return (st.st_mode & _S_IFREG) != 0;
}

int nei_path_is_dir(const char *path) {
    if (path == NULL) return 0;
    wchar_t wpath[4096];
    struct _stat64 st;
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return 0;
    if (_wstat64(wpath, &st) != 0) return 0;
    return (st.st_mode & _S_IFDIR) != 0;
}

int nei_path_is_readable(const char *path) {
    if (path == NULL) return 0;
    wchar_t wpath[4096];
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return 0;
    return _waccess(wpath, 04) == 0;
}

int nei_path_is_writable(const char *path) {
    if (path == NULL) return 0;
    wchar_t wpath[4096];
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return 0;
    return _waccess(wpath, 02) == 0;
}

int nei_path_is_executable(const char *path) {
    if (path == NULL) return 0;
    /* On Windows, check for executable extensions. */
    const char *name = path;
    const char *sep  = NULL;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') sep = p;
    }
    if (sep != NULL) name = sep + 1;

    const char *ext = strrchr(name, '.');
    if (ext == NULL) return 0;

    return (_stricmp(ext, ".exe") == 0 ||
            _stricmp(ext, ".bat") == 0 ||
            _stricmp(ext, ".cmd") == 0 ||
            _stricmp(ext, ".com") == 0);
}

int nei_path_create_dir(const char *path, int parents) {
    if (path == NULL || path[0] == '\0') return -1;

    wchar_t wpath[4096];
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return -1;

    if (!parents) {
        return _wmkdir(wpath) == 0 ? 0 : -1;
    }

    /* Recursive: walk up and create intermediate directories. */
    wchar_t tmp[4096];
    wcsncpy_s(tmp, 4096, wpath, _TRUNCATE);

    /* Skip drive letter / UNC root. */
    wchar_t *p = tmp;
    if (p[0] != L'\0' && p[1] == L':') p += 2;  /* "C:" */
    while (*p == L'\\' || *p == L'/') ++p;

    for (;;) {
        while (*p != L'\0' && *p != L'\\' && *p != L'/') ++p;
        wchar_t saved = *p;
        *p = L'\0';

        if (tmp[0] != L'\0') {
            char tmp_u8[4096];
            if (nei_wstr_to_utf8(tmp, -1, tmp_u8, 4096) < 0) return -1;
            if (!nei_path_is_dir(tmp_u8)) {
                if (_wmkdir(tmp) != 0 &&
                    GetLastError() != ERROR_ALREADY_EXISTS) return -1;
            }
        }

        if (saved == L'\0') break;
        *p = saved;
        ++p;
    }
    return 0;
}

int nei_path_remove(const char *path, int recursive) {
    if (path == NULL) return -1;

    wchar_t wpath[4096];
    if (nei_utf8_to_wstr(path, wpath, 4096) < 0) return -1;

    if (!nei_path_is_dir(path)) {
        return _wremove(wpath) == 0 ? 0 : -1;
    }

    if (!recursive) {
        return _wrmdir(wpath) == 0 ? 0 : -1;
    }

    /* Recursive directory removal. */
    wchar_t search[4096];
    wcscpy_s(search, 4096, wpath);
    wcscat_s(search, 4096, L"\\*");

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    int ret = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0) continue;

        wchar_t child[4096];
        wcscpy_s(child, 4096, wpath);
        wcscat_s(child, 4096, L"\\");
        wcscat_s(child, 4096, fd.cFileName);

        char child_u8[4096];
        if (nei_wstr_to_utf8(child, -1, child_u8, 4096) < 0) {
            ret = -1; break;
        }
        if (nei_path_remove(child_u8, 1) != 0) {
            ret = -1; break;
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    if (ret == 0) ret = (_wrmdir(wpath) == 0 ? 0 : -1);
    return ret;
}
