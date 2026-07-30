#include <nei/sys/fs_util.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

int nei_is_file_busy(const char *path) {
  int fd;
  int rc;
  int saved_errno;

  if (path == NULL)
    return -1;

  /* Step 1: advisory lock test (catches cooperative flock users). */
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;

  rc = flock(fd, LOCK_EX | LOCK_NB);
  if (rc != 0) {
    saved_errno = errno;
    close(fd);
    if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
      return 1;
    }
    return -1;
  }
  flock(fd, LOCK_UN);
  close(fd);

  /* Step 2: write-open test.
   *
   * On some systems opening an executable that is currently running
   * fails with ETXTBSY.  Opening a regular data file for writing
   * generally succeeds even if another process has it open, so this
   * is a best-effort complementary check. */
  fd = open(path, O_WRONLY);
  if (fd < 0) {
    if (errno == ETXTBSY)
      return 1;
    /* Other errors (EACCES, EROFS, …) do not imply "in use". */
    return 0;
  }
  close(fd);
  return 0;
}
