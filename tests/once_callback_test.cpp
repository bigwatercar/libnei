#include <gtest/gtest.h>

#include <neixx/common/location.h>
#include <neixx/functional/bind.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_runner.h>  // for OnceCallback<void()> alias

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace nei {
namespace {

// =============================================================================
// OnceCallback construction + Run
// =============================================================================

TEST(OnceCallbackTest, DefaultConstructedIsNull) {
  OnceCallback<void()> cb;
  EXPECT_FALSE(cb);
}

TEST(OnceCallbackTest, LambdaVoid) {
  int called = 0;
  OnceCallback<void()> cb = [&called]() { called = 1; };
  EXPECT_TRUE(cb);
  std::move(cb).Run();
  EXPECT_EQ(called, 1);
}

TEST(OnceCallbackTest, LambdaWithIntParam) {
  int result = 0;
  OnceCallback<void(int)> cb = [&result](int x) { result = x; };
  EXPECT_TRUE(cb);
  std::move(cb).Run(42);
  EXPECT_EQ(result, 42);
}

TEST(OnceCallbackTest, LambdaWithStringParam) {
  std::string captured;
  OnceCallback<void(std::string)> cb = [&captured](std::string s) {
    captured = std::move(s);
  };
  std::move(cb).Run("hello");
  EXPECT_EQ(captured, "hello");
}

TEST(OnceCallbackTest, LambdaWithConstRefParam) {
  std::string received;
  OnceCallback<void(const std::string&)> cb = [&received](const std::string& s) {
    received = s;
  };
  std::string input = "world";
  std::move(cb).Run(input);
  EXPECT_EQ(received, "world");
}

TEST(OnceCallbackTest, LambdaWithMoveOnlyParam) {
  std::unique_ptr<int> received;
  OnceCallback<void(std::unique_ptr<int>)> cb =
      [&received](std::unique_ptr<int> p) {
        received = std::move(p);
      };
  auto p = std::make_unique<int>(99);
  std::move(cb).Run(std::move(p));
  ASSERT_TRUE(received);
  EXPECT_EQ(*received, 99);
}

TEST(OnceCallbackTest, LambdaWithMultipleParams) {
  int sum = 0;
  std::string concat;
  OnceCallback<void(int, const std::string&)> cb =
      [&sum, &concat](int x, const std::string& s) {
        sum = x;
        concat = s;
      };
  std::move(cb).Run(10, "test");
  EXPECT_EQ(sum, 10);
  EXPECT_EQ(concat, "test");
}

// =============================================================================
// OnceCallback move semantics
// =============================================================================

TEST(OnceCallbackTest, MoveConstructor) {
  int called = 0;
  OnceCallback<void()> cb1 = [&called]() { called = 1; };
  OnceCallback<void()> cb2 = std::move(cb1);
  EXPECT_FALSE(cb1);  // moved-from
  EXPECT_TRUE(cb2);
  std::move(cb2).Run();
  EXPECT_EQ(called, 1);
}

TEST(OnceCallbackTest, MoveAssignment) {
  int called = 0;
  OnceCallback<void(int)> cb1 = [&called](int x) { called = x; };
  OnceCallback<void(int)> cb2;
  cb2 = std::move(cb1);
  EXPECT_FALSE(cb1);
  EXPECT_TRUE(cb2);
  std::move(cb2).Run(7);
  EXPECT_EQ(called, 7);
}

TEST(OnceCallbackTest, DestroyWithoutRunning) {
  int destroyed = 0;
  {
    auto guard = std::make_unique<int>(42);
    OnceCallback<void()> cb = [g = std::move(guard), &destroyed]() {
      destroyed = *g;
    };
    EXPECT_TRUE(cb);
    // cb destroyed here — functor should be properly cleaned up
  }
  EXPECT_EQ(destroyed, 0);  // never ran
}

// =============================================================================
// OnceCallback run-once semantics
// =============================================================================

TEST(OnceCallbackTest, RunConsumesCallback) {
  OnceCallback<void(int)> cb = [](int x) { (void)x; };
  EXPECT_TRUE(cb);
  std::move(cb).Run(1);
  EXPECT_FALSE(cb);  // consumed
}

// =============================================================================
// OnceCallback with BindOnce
// =============================================================================

void FreeFunction(int* out, int x) {
  *out = x;
}

TEST(OnceCallbackTest, BindOnceVoid) {
  int result = 0;
  OnceCallback<void()> cb = BindOnce(&FreeFunction, &result, 5);
  EXPECT_TRUE(cb);
  std::move(cb).Run();  // no args needed — bound via BindOnce
  EXPECT_EQ(result, 5);
}

// =============================================================================
// OnceCallback SBO / heap boundary
// =============================================================================

struct LargeFunctor {
  char padding[128];  // exceeds SBO size (48 bytes)
  int* called;
  void operator()() const { *called = 1; }
};

TEST(OnceCallbackTest, LargeFunctorFallsBackToHeap) {
  int called = 0;
  OnceCallback<void()> cb = LargeFunctor{{}, &called};
  EXPECT_TRUE(cb);
  std::move(cb).Run();
  EXPECT_EQ(called, 1);
}

// =============================================================================
// OnceCallback with OnceCallback<void()> alias
// =============================================================================

TEST(OnceCallbackTest, OnceCallbackVoidAlias) {
  int called = 0;
  OnceCallback<void()> cb = [&called]() { called = 1; };
  std::move(cb).Run();
  EXPECT_EQ(called, 1);
}

}  // namespace
}  // namespace nei
