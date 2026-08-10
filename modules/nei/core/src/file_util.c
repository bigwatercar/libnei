#include <nei/core/file_util.h>

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <nei/core/path_util.h>

int nei_file_exists(const char *path) {
  return nei_path_exists(path);
}

int nei_file_truncate(const char *path) {
  FILE *fp = nei_fopen_utf8(path, "wb");
  if (!fp)
    return -1;
  fclose(fp);
  return 0;
}

int64_t nei_file_append(const char *path, const void *buf, size_t len) {
  FILE *fp = nei_fopen_utf8(path, "ab");
  if (!fp)
    return -1;

  const unsigned char *p = (const unsigned char *)buf;
  size_t remaining = len;
  while (remaining > 0) {
    size_t n = fwrite(p, 1, remaining, fp);
    if (n == 0) {
      fclose(fp);
      return (int64_t)(len - remaining);
    }
    p += n;
    remaining -= n;
  }
  fclose(fp);
  return (int64_t)len;
}

int64_t nei_file_write(const char *path, const void *buf, size_t len) {
  FILE *fp = nei_fopen_utf8(path, "wb");
  if (!fp)
    return -1;

  const unsigned char *p = (const unsigned char *)buf;
  size_t remaining = len;
  while (remaining > 0) {
    size_t n = fwrite(p, 1, remaining, fp);
    if (n == 0) {
      fclose(fp);
      return (int64_t)(len - remaining);
    }
    p += n;
    remaining -= n;
  }
  fclose(fp);
  return (int64_t)len;
}

int64_t nei_file_read(const char *path, int64_t offset, void *buf, size_t len) {
  FILE *fp = nei_fopen_utf8(path, "rb");
  if (!fp)
    return -1;

#ifdef _WIN32
  if (_fseeki64(fp, offset, SEEK_SET) != 0) {
#else
  if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
#endif
    fclose(fp);
    return -1;
  }

  size_t n = fread(buf, 1, len, fp);
  fclose(fp);
  return (int64_t)n;
}
