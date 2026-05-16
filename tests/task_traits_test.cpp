#include <gtest/gtest.h>

#include <neixx/task/task_traits.h>

namespace nei {
namespace {

TEST(TaskTraitsTest, CopyAndMoveDoNotUseVariadicApply) {
  TaskTraits original(MayBlock(), TaskPriority::USER_BLOCKING,
                      TaskShutdownBehavior::kDrop);

  TaskTraits copied(original);
  EXPECT_TRUE(copied.may_block());
  EXPECT_EQ(copied.priority(), TaskPriority::USER_BLOCKING);
  EXPECT_EQ(copied.shutdown_behavior(), TaskShutdownBehavior::kDrop);

  TaskTraits moved(std::move(original));
  EXPECT_TRUE(moved.may_block());
  EXPECT_EQ(moved.priority(), TaskPriority::USER_BLOCKING);
  EXPECT_EQ(moved.shutdown_behavior(), TaskShutdownBehavior::kDrop);
}

TEST(TaskTraitsTest, VariadicCtorStillAcceptsDeclarativeArgs) {
  TaskTraits traits(MayBlock(), TaskPriority::BEST_EFFORT,
                    TaskShutdownBehavior::kDrop);

  EXPECT_TRUE(traits.may_block());
  EXPECT_EQ(traits.priority(), TaskPriority::BEST_EFFORT);
  EXPECT_EQ(traits.shutdown_behavior(), TaskShutdownBehavior::kDrop);
}

}  // namespace
}  // namespace nei
