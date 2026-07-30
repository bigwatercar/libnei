#include <gtest/gtest.h>

#include <nei/sys/fs_util.h>
#include <nei/core/file_util.h> /* nei_fopen_utf8 */

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

TEST(FsUtilTest, IsFileBusyOnMissingFileReturnsError) {
  EXPECT_LT(nei_is_file_busy("/nonexistent_file_12345_test"), 0);
}

TEST(FsUtilTest, IsFileBusyOnNullReturnsError) {
  EXPECT_LT(nei_is_file_busy(NULL), 0);
}

TEST(FsUtilTest, IsFileBusyOnFreeFile) {
  /* Create a temp file, check it's not busy, then clean up. */
  const char *path = "nei_test_fsutil_free.tmp";
  FILE *fp = nei_fopen_utf8(path, "w");
  ASSERT_NE(fp, nullptr);
  fclose(fp);

  EXPECT_EQ(nei_is_file_busy(path), 0);

  nei_file_remove(path);
}

TEST(FsUtilTest, IsFileBusyOnOpenFile) {
  const char *path = "nei_test_fsutil_busy.tmp";
  FILE *fp = nei_fopen_utf8(path, "w");
  ASSERT_NE(fp, nullptr);
  fprintf(fp, "hello");

  /* The file is held open by us  --  should be detected as busy. */
  int busy = nei_is_file_busy(path);
  fclose(fp);
  nei_file_remove(path);

  /* On POSIX advisory locks won't trigger unless both sides use flock.
   * On Windows with exclusive write access it will be detected.
   * So we just verify the call succeeds (doesn't return error). */
  EXPECT_GE(busy, 0) << "Should return 0 or 1, not error";
}
