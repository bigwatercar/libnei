#include <gtest/gtest.h>

#ifdef _WIN32

#include <nei/sys/win/utils.h>

#include <filesystem>
#include <string>

/* =========================================================================
 * resolve_shortcut
 * ========================================================================= */

TEST(WinUtilsTest, ResolveShortcutNullReturnsError) {
    EXPECT_LT(nei_win_resolve_shortcut(nullptr, nullptr, 0), 0);
}

TEST(WinUtilsTest, ResolveShortcutMissingReturnsError) {
    char buf[1024];
    int ret = nei_win_resolve_shortcut(
        "C:\\nonexistent_shortcut_12345.lnk", buf, sizeof(buf));
    EXPECT_LT(ret, 0);
}

TEST(WinUtilsTest, ResolveShortcutCppWrapperCompiles) {
    /* Verify the C++ wrapper compiles and returns empty on failure. */
    auto path = nei_win_resolve_shortcut(
        "C:\\nonexistent_shortcut_12345.lnk");
    EXPECT_TRUE(path.empty());
}

#endif /* _WIN32 */
