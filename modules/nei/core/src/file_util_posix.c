#include <nei/core/file_util.h>
#include <nei/core/encoding.h>

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

FILE *nei_fopen_utf8(const char *path, const char *mode) {
  return fopen(path, mode);
}

int nei_file_size(const char *path, uint64_t *out_size) {
  struct stat st;
  if (out_size == NULL || stat(path, &st) != 0)
    return -1;
  if (!S_ISREG(st.st_mode))
    return -1;
  *out_size = (uint64_t)st.st_size;
  return 0;
}

int nei_file_remove(const char *path) {
  return remove(path) == 0 ? 0 : -1;
}

int nei_file_rename(const char *old_path, const char *new_path) {
  return rename(old_path, new_path) == 0 ? 0 : -1;
}
