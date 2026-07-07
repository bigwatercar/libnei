// ---------------------------------------------------------------------------
// Custom test main  --  provides an AtExitManager for the entire test process.
//
// Many neixx components (IOBufferPool via Singleton<>, etc.) require an active
// AtExitManager at initialization time.  Google Test's default main() does not
// create one, causing CHECK failures in Singleton<>::GetInstance().
//
// This file replaces GTest::gtest_main and installs a process-wide
// AtExitManager before any test code runs.
// ---------------------------------------------------------------------------

#include <neixx/common/at_exit.h>

#include <gtest/gtest.h>

int main(int argc, char** argv) {
  // The AtExitManager MUST be the first stack object so it outlives all
  // singletons created during test execution.  Without it, any
  // Singleton<T>::GetInstance() call will CHECK-fail.
  nei::AtExitManager at_exit;

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
