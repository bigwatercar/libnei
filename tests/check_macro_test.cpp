#include <gtest/gtest.h>

#include <nei/debug/check.h>

namespace {

// Death test fixture to ensure proper test isolation
// Note: Running death tests in a multithreaded context may cause
// GoogleTest warnings about fork() safety. These can be suppressed
// by running tests with: --gtest_death_test_style=threadsafe
class CheckMacroCDeathTest : public ::testing::Test {};

} // namespace

TEST(CheckMacroCTest, CheckPassDoesNotCrash) {
  CHECK(1 == 1);
  CHECK_EQ(2, 2);
  CHECK_NE(2, 3);
  CHECK_LT(2, 3);
  CHECK_LE(2, 2);
  CHECK_GT(3, 2);
  CHECK_GE(2, 2);
}

TEST_F(CheckMacroCDeathTest, CheckFailureCrashes) {
  EXPECT_DEATH({ CHECK(false); }, ".*");
}

TEST_F(CheckMacroCDeathTest, DCheckFailureCrashes) {
#if NEI_DCHECK_IS_ON
  EXPECT_DEATH({ DCHECK_EQ(1, 2); }, ".*");
#else
  DCHECK_EQ(1, 2);
  SUCCEED();
#endif
}

TEST(CheckMacroCTest, DCheckEvaluationFollowsSwitch) {
  int eval_count = 0;
  DCHECK(++eval_count == 1);
#if NEI_DCHECK_IS_ON
  EXPECT_EQ(eval_count, 1);
#else
  EXPECT_EQ(eval_count, 0);
#endif
}
