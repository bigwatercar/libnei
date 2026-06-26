#include <nei/core/path_util.h>
#include <nei/core/encoding.h>

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int nei_path_exists(const char *path) {
    if (path == NULL) return 0;
    return access(path, F_OK) == 0;
}

int nei_path_is_file(const char *path) {
    if (path == NULL) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

int nei_path_is_dir(const char *path) {
    if (path == NULL) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

int nei_path_is_readable(const char *path) {
    if (path == NULL) return 0;
    return access(path, R_OK) == 0;
}

int nei_path_is_writable(const char *path) {
    if (path == NULL) return 0;
    return access(path, W_OK) == 0;
}

int nei_path_is_executable(const char *path) {
    if (path == NULL) return 0;
    return access(path, X_OK) == 0;
}

int nei_path_create_dir(const char *path, int parents) {
    if (path == NULL || path[0] == '\0') return -1;

    if (!parents) {
        return mkdir(path, 0755) == 0 ? 0 : -1;
    }

    /* Recursive: walk up and create intermediate directories. */
    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);

    /* Skip leading separators. */
    char *p = tmp;
    while (*p == '/') ++p;

    for (;;) {
        /* Find next separator. */
        while (*p != '\0' && *p != '/') ++p;
        char saved = *p;
        *p = '\0';

        if (tmp[0] != '\0' && !nei_path_is_dir(tmp)) {
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        }

        if (saved == '\0') break;
        *p = '/';
        ++p;
    }
    return 0;
}

int nei_path_remove(const char *path, int recursive) {
    if (path == NULL) return -1;

    if (!nei_path_is_dir(path)) {
        return unlink(path) == 0 ? 0 : -1;
    }

    if (!recursive) {
        return rmdir(path) == 0 ? 0 : -1;
    }

    /* Recursive directory removal. */
    DIR *dir = opendir(path);
    if (dir == NULL) return -1;

    struct dirent *entry;
    int ret = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;

        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { ret = -1; break; }

        if (nei_path_remove(child, 1) != 0) { ret = -1; break; }
    }
    closedir(dir);

    if (ret == 0) ret = (rmdir(path) == 0 ? 0 : -1);
    return ret;
}
