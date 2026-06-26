#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <nei/core/path_util.h>

/* =========================================================================
 * is_separator
 * ========================================================================= */

TEST(PathUtilTest, IsSeparator) {
    EXPECT_NE(nei_path_is_separator('/'), 0);
    EXPECT_NE(nei_path_is_separator('\\'), 0);
    EXPECT_EQ(nei_path_is_separator('a'), 0);
    EXPECT_EQ(nei_path_is_separator('.'), 0);
    EXPECT_EQ(nei_path_is_separator('\0'), 0);
}

/* =========================================================================
 * is_absolute
 * ========================================================================= */

TEST(PathUtilTest, IsAbsolute) {
    EXPECT_NE(nei_path_is_absolute("/a/b"), 0);
    EXPECT_NE(nei_path_is_absolute("\\a\\b"), 0);
    EXPECT_NE(nei_path_is_absolute("C:\\a"), 0);
    EXPECT_NE(nei_path_is_absolute("C:/a"), 0);
    EXPECT_EQ(nei_path_is_absolute("a/b"), 0);
    EXPECT_EQ(nei_path_is_absolute(""), 0);
}

/* =========================================================================
 * join
 * ========================================================================= */

TEST(PathUtilTest, JoinBasic) {
    char buf[256];
    int n = nei_path_join(buf, sizeof(buf), "a", "b");
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "a/b");
}

TEST(PathUtilTest, JoinAbsoluteReplaces) {
    char buf[256];
    int n = nei_path_join(buf, sizeof(buf), "a", "/b");
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "/b");
}

TEST(PathUtilTest, JoinTrailingSlash) {
    char buf[256];
    int n = nei_path_join(buf, sizeof(buf), "a/", "b");
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "a/b");  /* no double slash */
}

TEST(PathUtilTest, JoinSmallBuffer) {
    char buf[4];
    int n = nei_path_join(buf, sizeof(buf), "abc", "def");
    EXPECT_GT(n, 0);
    EXPECT_EQ((int)strlen(buf), 3);
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

/* =========================================================================
 * dirname
 * ========================================================================= */

TEST(PathUtilTest, DirnameBasic) {
    char buf[256];
    int n = nei_path_dirname("/a/b/c.txt", buf, sizeof(buf));
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "/a/b");
}

TEST(PathUtilTest, DirnameNoSep) {
    char buf[256];
    nei_path_dirname("file.txt", buf, sizeof(buf));
    EXPECT_STREQ(buf, ".");
}

TEST(PathUtilTest, DirnameRoot) {
    char buf[256];
    nei_path_dirname("/", buf, sizeof(buf));
    EXPECT_STREQ(buf, "/");
}

TEST(PathUtilTest, DirnameTrailingSlash) {
    char buf[256];
    nei_path_dirname("/a/b/", buf, sizeof(buf));
    EXPECT_STREQ(buf, "/a");
}

/* =========================================================================
 * basename
 * ========================================================================= */

TEST(PathUtilTest, BasenameBasic) {
    char buf[256];
    nei_path_basename("/a/b/c.txt", buf, sizeof(buf));
    EXPECT_STREQ(buf, "c.txt");
}

TEST(PathUtilTest, BasenameNoSep) {
    char buf[256];
    nei_path_basename("file.txt", buf, sizeof(buf));
    EXPECT_STREQ(buf, "file.txt");
}

TEST(PathUtilTest, BasenameTrailingSlash) {
    char buf[256];
    nei_path_basename("/a/b/", buf, sizeof(buf));
    EXPECT_STREQ(buf, "b");
}

/* =========================================================================
 * stem
 * ========================================================================= */

TEST(PathUtilTest, StemSingleExt) {
    char buf[256];
    nei_path_stem("/a/b/file.txt", buf, sizeof(buf));
    EXPECT_STREQ(buf, "file");
}

TEST(PathUtilTest, StemMultiExt) {
    char buf[256];
    nei_path_stem("/a/b/archive.tar.gz", buf, sizeof(buf));
    EXPECT_STREQ(buf, "archive");
}

TEST(PathUtilTest, StemNoExt) {
    char buf[256];
    nei_path_stem("/a/b/Makefile", buf, sizeof(buf));
    EXPECT_STREQ(buf, "Makefile");
}

TEST(PathUtilTest, StemHiddenFile) {
    char buf[256];
    /* .gitignore — base name starts with dot, no "extension" */
    nei_path_stem("/a/b/.gitignore", buf, sizeof(buf));
    EXPECT_STREQ(buf, ".gitignore");
}

/* =========================================================================
 * extension
 * ========================================================================= */

TEST(PathUtilTest, ExtensionSingle) {
    char buf[256];
    nei_path_extension("/a/b/file.txt", buf, sizeof(buf));
    EXPECT_STREQ(buf, ".txt");
}

TEST(PathUtilTest, ExtensionMulti) {
    char buf[256];
    nei_path_extension("/a/b/archive.tar.gz", buf, sizeof(buf));
    EXPECT_STREQ(buf, ".gz");
}

TEST(PathUtilTest, ExtensionNone) {
    char buf[256];
    nei_path_extension("/a/b/Makefile", buf, sizeof(buf));
    EXPECT_STREQ(buf, "");
}

TEST(PathUtilTest, ExtensionHiddenFile) {
    char buf[256];
    nei_path_extension("/a/b/.gitignore", buf, sizeof(buf));
    EXPECT_STREQ(buf, "");
}

/* =========================================================================
 * extensions (all)
 * ========================================================================= */

TEST(PathUtilTest, ExtensionsSingle) {
    char buf[256];
    nei_path_extensions("/a/b/file.txt", buf, sizeof(buf));
    EXPECT_STREQ(buf, ".txt");
}

TEST(PathUtilTest, ExtensionsMulti) {
    char buf[256];
    nei_path_extensions("/a/b/archive.tar.gz", buf, sizeof(buf));
    EXPECT_STREQ(buf, ".tar.gz");
}

TEST(PathUtilTest, ExtensionsNone) {
    char buf[256];
    nei_path_extensions("/a/b/Makefile", buf, sizeof(buf));
    EXPECT_STREQ(buf, "");
}

TEST(PathUtilTest, ExtensionsHiddenFile) {
    char buf[256];
    nei_path_extensions("/a/b/.gitignore", buf, sizeof(buf));
    EXPECT_STREQ(buf, "");
}

/* =========================================================================
 * normalize
 * ========================================================================= */

TEST(PathUtilTest, NormalizeDots) {
    char buf[256];
    nei_path_normalize("/a/./b/../c", buf, sizeof(buf));
    EXPECT_STREQ(buf, "/a/c");
}

TEST(PathUtilTest, NormalizeDoubleSlash) {
    char buf[256];
    nei_path_normalize("/a//b", buf, sizeof(buf));
    EXPECT_STREQ(buf, "/a/b");
}

TEST(PathUtilTest, NormalizeAboveRoot) {
    char buf[256];
    nei_path_normalize("/a/../../b", buf, sizeof(buf));
    EXPECT_STREQ(buf, "/b");
}

TEST(PathUtilTest, NormalizeEmpty) {
    char buf[256];
    nei_path_normalize("", buf, sizeof(buf));
    EXPECT_STREQ(buf, ".");
}

/* =========================================================================
 * to_native
 * ========================================================================= */

TEST(PathUtilTest, ToNativeMixedSlashes) {
    char buf[] = "a/b\\c/d";
    nei_path_to_native(buf);
#ifdef _WIN32
    EXPECT_STREQ(buf, "a\\b\\c\\d");
#else
    /* POSIX: backslash is a valid filename character — unchanged. */
    EXPECT_STREQ(buf, "a/b\\c/d");
#endif
}

TEST(PathUtilTest, ToNativeAlreadyNative) {
    char buf[] = "a\\b\\c";
    nei_path_to_native(buf);
#ifdef _WIN32
    EXPECT_STREQ(buf, "a\\b\\c");
#else
    EXPECT_STREQ(buf, "a\\b\\c");
#endif
}

TEST(PathUtilTest, ToNativeNullReturnsError) {
    EXPECT_LT(nei_path_to_native(NULL), 0);
}

/* =========================================================================
 * file-system queries
 * ========================================================================= */

TEST(PathUtilTest, ExistsCurrentDir) {
    EXPECT_NE(nei_path_exists("."), 0);
}

TEST(PathUtilTest, IsDirCurrent) {
    EXPECT_NE(nei_path_is_dir("."), 0);
    EXPECT_EQ(nei_path_is_file("."), 0);
}

TEST(PathUtilTest, ExistsMissingReturnsFalse) {
    EXPECT_EQ(nei_path_exists("/nonexistent_path_12345_test"), 0);
}

TEST(PathUtilTest, IsFileMissingReturnsFalse) {
    EXPECT_EQ(nei_path_is_file("/nonexistent_path_12345_test"), 0);
}

TEST(PathUtilTest, IsDirMissingReturnsFalse) {
    EXPECT_EQ(nei_path_is_dir("/nonexistent_path_12345_test"), 0);
}

TEST(PathUtilTest, NullReturnsFalse) {
    EXPECT_EQ(nei_path_exists(NULL), 0);
    EXPECT_EQ(nei_path_is_file(NULL), 0);
    EXPECT_EQ(nei_path_is_dir(NULL), 0);
    EXPECT_EQ(nei_path_is_readable(NULL), 0);
    EXPECT_EQ(nei_path_is_writable(NULL), 0);
    EXPECT_EQ(nei_path_is_executable(NULL), 0);
}

/* =========================================================================
 * create_dir / remove
 * ========================================================================= */

#include <nei/sys/host_info.h>   /* for nei_get_temp_dir */
#include <nei/core/file_util.h>  /* for nei_fopen_utf8 */

TEST(PathUtilTest, CreateDirSimple) {
    char base[256];
    nei_get_temp_dir(base, sizeof(base));
    char path[512];
    snprintf(path, sizeof(path), "%snei_test_path_create_simple", base);

    /* Clean up from previous runs. */
    nei_path_remove(path, 1);

    EXPECT_EQ(nei_path_create_dir(path, 0), 0);
    EXPECT_NE(nei_path_is_dir(path), 0);

    /* Clean up. */
    EXPECT_EQ(nei_path_remove(path, 0), 0);
    EXPECT_EQ(nei_path_exists(path), 0);
}

TEST(PathUtilTest, CreateDirParents) {
    char base[256];
    nei_get_temp_dir(base, sizeof(base));
    char path[512];
    snprintf(path, sizeof(path), "%snei_test_parents/a/b/c", base);

    nei_path_remove(path, 1); /* clean up */
    /* Also clean intermediate dirs */
    {
        char p2[512];
        snprintf(p2, sizeof(p2), "%snei_test_parents", base);
        nei_path_remove(p2, 1);
    }

    EXPECT_EQ(nei_path_create_dir(path, 1), 0);
    EXPECT_NE(nei_path_is_dir(path), 0);

    /* Clean up from base. */
    char p2[512];
    snprintf(p2, sizeof(p2), "%snei_test_parents", base);
    EXPECT_EQ(nei_path_remove(p2, 1), 0);
    EXPECT_EQ(nei_path_exists(p2), 0);
}

TEST(PathUtilTest, RemoveRecursive) {
    char base[256];
    nei_get_temp_dir(base, sizeof(base));
    char dir[512];
    snprintf(dir, sizeof(dir), "%snei_test_remove_rec", base);

    nei_path_remove(dir, 1); /* clean up */
    ASSERT_EQ(nei_path_create_dir(dir, 1), 0);

    /* Create a file inside. */
    char file[512];
    snprintf(file, sizeof(file), "%s/test.txt", dir);
    FILE *fp = nei_fopen_utf8(file, "w");
    ASSERT_NE(fp, nullptr);
    fclose(fp);

    EXPECT_NE(nei_path_is_file(file), 0);

    /* Non-recursive remove should fail on non-empty dir. */
    EXPECT_NE(nei_path_remove(dir, 0), 0);
    EXPECT_NE(nei_path_exists(dir), 0);

    /* Recursive remove should succeed. */
    EXPECT_EQ(nei_path_remove(dir, 1), 0);
    EXPECT_EQ(nei_path_exists(dir), 0);
}

TEST(PathUtilTest, CreateDirNullReturnsError) {
    EXPECT_LT(nei_path_create_dir(NULL, 0), 0);
}

TEST(PathUtilTest, RemoveNullReturnsError) {
    EXPECT_LT(nei_path_remove(NULL, 0), 0);
}
