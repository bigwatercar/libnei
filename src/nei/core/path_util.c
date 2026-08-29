#include <nei/core/path_util.h>

#include <string.h>

int nei_path_is_separator(char c) {
  return c == '/' || c == '\\';
}

int nei_path_is_absolute(const char *path) {
  if (path == NULL || path[0] == '\0')
    return 0;
  /* POSIX-style root */
  if (path[0] == '/' || path[0] == '\\')
    return 1;
  /* Windows drive letter: [A-Za-z]:\ or [A-Za-z]:/ */
  if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
    return 1;
  }
  return 0;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Copy up to size-1 bytes, always null-terminate.  Returns full length
 * (excluding null) that would have been written. */
static int buf_copy(char *dst, size_t size, const char *src, size_t len) {
  if (size == 0)
    return (int)len;
  size_t copy = (len < size - 1) ? len : (size - 1);
  memcpy(dst, src, copy);
  dst[copy] = '\0';
  return (int)len;
}

/* Find the last path separator in [p, end).  Returns NULL if none found. */
static const char *find_last_sep(const char *p, const char *end) {
  const char *sep = NULL;
  while (p < end) {
    if (nei_path_is_separator(*p))
      sep = p;
    ++p;
  }
  return sep;
}

/* =========================================================================
 * Public API  --  string operations
 * ========================================================================= */

int nei_path_join(char *dst, size_t size, const char *a, const char *b) {
  if (dst == NULL || a == NULL || b == NULL)
    return -1;
  if (size == 0) {
    /* Calculate required size without writing. */
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    if (nei_path_is_absolute(b))
      return (int)(blen + 1);
    /* +1 for separator, +1 for null */
    return (int)(alen + 1 + blen + 1);
  }

  /* If b is absolute, just use b. */
  if (nei_path_is_absolute(b)) {
    size_t blen = strlen(b);
    return buf_copy(dst, size, b, blen);
  }

  size_t alen = strlen(a);
  size_t blen = strlen(b);
  size_t total = alen + 1 + blen; /* a + '/' + b */
  if (total >= size) {
    /* Truncate: write what fits. */
    size_t pos = 0;
    if (pos < size - 1) {
      size_t ac = (alen < size - 1 - pos) ? alen : (size - 1 - pos);
      memcpy(dst + pos, a, ac);
      pos += ac;
    }
    if (alen > 0 && !nei_path_is_separator(a[alen - 1]) && pos < size - 1) {
      dst[pos++] = '/';
    }
    if (pos < size - 1) {
      size_t bc = (blen < size - 1 - pos) ? blen : (size - 1 - pos);
      memcpy(dst + pos, b, bc);
      pos += bc;
    }
    dst[pos] = '\0';
    return (int)total;
  }

  /* Full copy. */
  size_t pos = 0;
  memcpy(dst + pos, a, alen);
  pos += alen;
  if (alen > 0 && !nei_path_is_separator(a[alen - 1])) {
    dst[pos++] = '/';
  }
  memcpy(dst + pos, b, blen);
  pos += blen;
  dst[pos] = '\0';
  return (int)pos;
}

int nei_path_dirname(const char *path, char *buf, size_t size) {
  if (path == NULL || buf == NULL)
    return -1;

  size_t len = strlen(path);
  if (len == 0)
    return buf_copy(buf, size, ".", 1);

  const char *end = path + len;
  /* Strip trailing separators, but keep root. */
  while (end > path + 1 && nei_path_is_separator(*(end - 1)))
    --end;

  const char *sep = find_last_sep(path, end);
  if (sep == NULL)
    return buf_copy(buf, size, ".", 1);

  /* If the only separator is at position 0, return root. */
  if (sep == path)
    return buf_copy(buf, size, path, 1);

  /* Windows drive letter root: "C:\" -> "C:\" */
  if (sep == path + 2 && path[1] == ':' && nei_path_is_separator(*sep)) {
    return buf_copy(buf, size, path, (size_t)(sep - path + 1));
  }

  /* Strip trailing separators from dir part too. */
  while (sep > path && nei_path_is_separator(*(sep - 1)))
    --sep;
  if (sep == path)
    return buf_copy(buf, size, path, 1);

  return buf_copy(buf, size, path, (size_t)(sep - path));
}

int nei_path_basename(const char *path, char *buf, size_t size) {
  if (path == NULL || buf == NULL)
    return -1;

  size_t len = strlen(path);
  if (len == 0)
    return buf_copy(buf, size, "", 0);

  const char *end = path + len;
  /* Strip trailing separators. */
  while (end > path + 1 && nei_path_is_separator(*(end - 1)))
    --end;

  const char *sep = find_last_sep(path, end);
  const char *start = sep ? (sep + 1) : path;

  return buf_copy(buf, size, start, (size_t)(end - start));
}

int nei_path_stem(const char *path, char *buf, size_t size) {
  if (path == NULL || buf == NULL)
    return -1;

  size_t len = strlen(path);
  if (len == 0)
    return buf_copy(buf, size, "", 0);

  const char *end = path + len;
  while (end > path + 1 && nei_path_is_separator(*(end - 1)))
    --end;

  const char *sep = find_last_sep(path, end);
  const char *start = sep ? (sep + 1) : path;

  /* Find the first dot (after any leading dots). */
  if (start >= end)
    return buf_copy(buf, size, "", 0);
  if (*start == '.') {
    /* Base name starts with a dot  --  skip leading dots, then look
     * for the first dot after that as the extension start. */
    const char *p = start + 1;
    while (p < end && *p == '.')
      ++p;
    const char *dot = NULL;
    if (p < end)
      dot = memchr(p, '.', (size_t)(end - p));
    if (dot != NULL)
      end = dot;
  } else {
    const char *dot = memchr(start, '.', (size_t)(end - start));
    if (dot != NULL)
      end = dot;
  }

  return buf_copy(buf, size, start, (size_t)(end - start));
}

int nei_path_extension(const char *path, char *buf, size_t size) {
  if (path == NULL || buf == NULL)
    return -1;

  size_t len = strlen(path);
  if (len == 0)
    return buf_copy(buf, size, "", 0);

  const char *end = path + len;
  while (end > path + 1 && nei_path_is_separator(*(end - 1)))
    --end;

  const char *sep = find_last_sep(path, end);
  const char *start = sep ? (sep + 1) : path;

  /* No dot, or base name starts with dot -> no extension. */
  if (start >= end || *start == '.')
    return buf_copy(buf, size, "", 0);

  const char *last_dot = NULL;
  for (const char *p = end - 1; p > start; --p) {
    if (*p == '.') {
      last_dot = p;
      break;
    }
  }
  if (last_dot == NULL)
    return buf_copy(buf, size, "", 0);

  return buf_copy(buf, size, last_dot, (size_t)(end - last_dot));
}

int nei_path_extensions(const char *path, char *buf, size_t size) {
  if (path == NULL || buf == NULL)
    return -1;

  size_t len = strlen(path);
  if (len == 0)
    return buf_copy(buf, size, "", 0);

  const char *end = path + len;
  while (end > path + 1 && nei_path_is_separator(*(end - 1)))
    --end;

  const char *sep = find_last_sep(path, end);
  const char *start = sep ? (sep + 1) : path;

  /* No dot, or base name starts with dot -> no extension. */
  if (start >= end || *start == '.')
    return buf_copy(buf, size, "", 0);

  const char *first_dot = memchr(start, '.', (size_t)(end - start));
  if (first_dot == NULL)
    return buf_copy(buf, size, "", 0);

  return buf_copy(buf, size, first_dot, (size_t)(end - first_dot));
}

int nei_path_normalize(const char *path, char *buf, size_t size) {
  if (path == NULL || buf == NULL)
    return -1;

  size_t len = strlen(path);
  if (len == 0)
    return buf_copy(buf, size, ".", 1);

  /* Use a stack buffer for the working copy. */
  char tmp[4096];
  if (len >= sizeof(tmp))
    len = sizeof(tmp) - 1;
  memcpy(tmp, path, len);
  tmp[len] = '\0';

  /* Determine root prefix. */
  const char *root = "";
  size_t root_len = 0;
  char *p = tmp;
  if (*p == '/' || *p == '\\') {
    root = "/";
    root_len = 1;
    ++p;
  } else if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
    root = tmp;
    root_len = 2;
    p += 2;
    if (*p == '/' || *p == '\\') {
      root_len = 3;
      ++p;
    }
  }

  /* Build normalised path into a temporary buffer, component by component. */
  char out[4096];
  size_t out_len = root_len;
  memcpy(out, root, root_len);

  while (*p != '\0') {
    /* Skip separators. */
    while (*p == '/' || *p == '\\')
      ++p;
    if (*p == '\0')
      break;

    /* Find next component. */
    const char *comp = p;
    while (*p != '\0' && *p != '/' && *p != '\\')
      ++p;
    size_t comp_len = (size_t)(p - comp);

    /* "."  --  skip. */
    if (comp_len == 1 && comp[0] == '.')
      continue;

    /* ".."  --  go up one level. */
    if (comp_len == 2 && comp[0] == '.' && comp[1] == '.') {
      if (out_len > root_len) {
        /* Remove last component. */
        --out_len;
        while (out_len > root_len && out[out_len - 1] != '/' && out[out_len - 1] != '\\')
          --out_len;
      }
      continue;
    }

    /* Append separator + component. */
    if (out_len > root_len && !nei_path_is_separator(out[out_len - 1])) {
      out[out_len++] = '/';
    }
    if (out_len + comp_len < sizeof(out)) {
      memcpy(out + out_len, comp, comp_len);
      out_len += comp_len;
    }
  }

  if (out_len == 0) {
    out[0] = '.';
    out[1] = '\0';
    out_len = 1;
  } else {
    out[out_len] = '\0';
  }

  return buf_copy(buf, size, out, out_len);
}

int nei_path_to_native(char *path) {
  if (path == NULL)
    return -1;

#ifdef _WIN32
  char from = '/', to = '\\';
  size_t len = 0;
  for (char *p = path; *p != '\0'; ++p) {
    if (*p == from)
      *p = to;
    ++len;
  }
  return (int)len;
#else
  /* POSIX: backslash is a valid filename character  --  do not alter. */
  return (int)strlen(path);
#endif
}
