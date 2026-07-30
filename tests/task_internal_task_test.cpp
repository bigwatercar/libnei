#include <gtest/gtest.h>

#include <neixx/common/time.h>
#include "internal/task.h"

namespace nei {
namespace internal {

namespace {

Task MakeTask(const TimeTicks& delayed_run_time,
              std::int64_t sequence_num,
              SequenceToken sequence_token = SequenceToken()) {
  return Task{
      OnceCallback<void()>(),
      Location{"task_internal_task_test.cpp", 1, "MakeTask"},
      TimeTicks(),
      delayed_run_time,
      sequence_num,
      sequence_token,
  };
}

}  // namespace

TEST(TaskInternalTest, OperatorGreaterUsesDelayedRunTimeFirst) {
  const Task lhs = MakeTask(TimeTicks::Now() + TimeDelta::FromMilliseconds(20), 1);
  const Task rhs = MakeTask(TimeTicks::Now() + TimeDelta::FromMilliseconds(10), 1000);

  EXPECT_TRUE(lhs > rhs);
  EXPECT_FALSE(rhs > lhs);
}

TEST(TaskInternalTest, OperatorGreaterUsesSequenceNumWhenTimesEqual) {
  const TimeTicks now = TimeTicks::Now();
  const Task lhs = MakeTask(now, 8);
  const Task rhs = MakeTask(now, 5);

  EXPECT_TRUE(lhs > rhs);
  EXPECT_FALSE(rhs > lhs);
}

TEST(TaskInternalTest, OperatorGreaterIgnoresOtherFields) {
  const TimeTicks now = TimeTicks::Now();
  const Task lhs = MakeTask(now, 99, SequenceToken::Create());
  const Task rhs = MakeTask(now, 99, SequenceToken::Create());

  EXPECT_FALSE(lhs > rhs);
  EXPECT_FALSE(rhs > lhs);
}

}  // namespace internal
}  // namespace nei
