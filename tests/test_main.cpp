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

#if defined(_WIN32)
#include <crtdbg.h>
#include <stdlib.h>
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  // gtest death tests (CHECK/DCHECK failures, FATAL log crashes, invalid
  // WeakPtr dereferences) deliberately abort() their child processes to
  // verify crash paths.  The debug CRT would otherwise pop an "abort() has
  // been called" dialog for every such child — especially jarring under a
  // debugger.  Suppress the dialog: abort() still terminates the process, so
  // gtest still observes the abnormal exit.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif

  // The AtExitManager MUST be the first stack object so it outlives all
  // singletons created during test execution.  Without it, any
  // Singleton<T>::GetInstance() call will CHECK-fail.
  nei::AtExitManager at_exit;

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
