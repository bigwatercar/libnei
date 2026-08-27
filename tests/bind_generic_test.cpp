#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <neixx/functional/bind.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/weak_ptr.h>

namespace nei {
namespace {

int FreeAdd(int a, int b) {
  return a + b;
}

int FreeSquare(int a) {
  return a * a;
}

// =============================================================================
// BindOnce — 非 void 返回值
// =============================================================================

TEST(BindGenericTest, BindOnceNonVoidReturn) {
  OnceCallback<int()> cb = BindOnce([]() { return 42; });
  EXPECT_TRUE(cb);
  EXPECT_EQ(42, std::move(cb).Run());
}

TEST(BindGenericTest, BindOnceUnboundArgument) {
  // 绑定 1 个参数，剩 1 个调用期参数。
  OnceCallback<int(int)> cb = BindOnce([](int a, int b) { return a + b; }, 10);
  EXPECT_EQ(15, std::move(cb).Run(5));
  // OnceCallback 已消费：再次 Run 返回默认值。
  EXPECT_EQ(0, std::move(cb).Run(1));
}

TEST(BindGenericTest, BindOnceFreeFunctionNonVoid) {
  OnceCallback<int(int)> cb = BindOnce(&FreeAdd, 35);
  EXPECT_EQ(42, std::move(cb).Run(7));

  OnceCallback<int()> sq = BindOnce(&FreeSquare, 9);
  EXPECT_EQ(81, std::move(sq).Run());
}

TEST(BindGenericTest, BindOnceMemberFunctionNonVoid) {
  struct Counter {
    int Multiply(int mult) const {
      return value * mult;
    }

    int value = 6;
  };

  Counter c;
  // 绑定 receiver + mult -> OnceCallback<int()>
  OnceCallback<int()> cb = BindOnce(&Counter::Multiply, &c, 7);
  EXPECT_EQ(42, std::move(cb).Run());

  // 只绑定 receiver -> OnceCallback<int(int)>
  OnceCallback<int(int)> cb2 = BindOnce(&Counter::Multiply, &c);
  EXPECT_EQ(48, std::move(cb2).Run(8));
}

TEST(BindGenericTest, BindOnceMoveOnlyReturn) {
  OnceCallback<std::unique_ptr<int>()> cb = BindOnce([]() { return std::make_unique<int>(7); });
  auto r = std::move(cb).Run();
  ASSERT_TRUE(r);
  EXPECT_EQ(7, *r);
}

TEST(BindGenericTest, BindOnceStdFunctionFunctor) {
  std::function<int(int, int)> f = [](int a, int b) { return a + b; };
  OnceCallback<int()> cb = BindOnce(std::move(f), 1, 2);
  EXPECT_EQ(3, std::move(cb).Run());
}

TEST(BindGenericTest, BindOnceCallbackAsFunctor) {
  // 回调作为 functor：BindOnce(cb, args...) 把已有回调再绑定若干参数。
  bool called = false;
  OnceCallback<void(bool)> inner = [&called](bool ok) { called = ok; };
  OnceCallback<void()> outer = BindOnce(std::move(inner), true);
  std::move(outer).Run();
  EXPECT_TRUE(called);

  // 带返回值的回调作为 functor。
  OnceCallback<int(int)> inc = [](int x) { return x + 1; };
  OnceCallback<int()> outer2 = BindOnce(std::move(inc), 41);
  EXPECT_EQ(42, std::move(outer2).Run());
}

TEST(BindGenericTest, BindOnceWeakPtrCancellationReturnsDefault) {
  struct Target {
    int Compute() const {
      return 99;
    }
  };

  auto target = std::make_unique<Target>();
  WeakPtrFactory<Target> factory(target.get());
  OnceCallback<int()> cb = BindOnce(&Target::Compute, factory.GetWeakPtr());
  factory.InvalidateWeakPtrs();
  EXPECT_EQ(0, std::move(cb).Run());
}

TEST(BindGenericTest, BindOnceReturnTypeDeduction) {
  // 直接推导（CTAD 风格）：无需显式写出回调类型。
  auto cb = BindOnce([]() { return 7; });
  static_assert(std::is_same_v<decltype(cb), OnceCallback<int()>>);
  EXPECT_EQ(7, std::move(cb).Run());
}

// =============================================================================
// BindRepeating — 非 void 返回值 + 未绑定参数
// =============================================================================

TEST(BindGenericTest, BindRepeatingNonVoidAndUnbound) {
  RepeatingCallback<int(int)> cb = BindRepeating([](int a, int b) { return a * b; }, 3);
  EXPECT_EQ(12, cb.Run(4));
  EXPECT_EQ(15, cb.Run(5));
}

TEST(BindGenericTest, BindRepeatingMemberFunctionNonVoid) {
  struct Counter {
    int Add(int delta) {
      value += delta;
      return value;
    }

    int value = 10;
  };

  Counter c;
  RepeatingCallback<int(int)> cb = BindRepeating(&Counter::Add, &c);
  EXPECT_EQ(13, cb.Run(3));
  EXPECT_EQ(16, cb.Run(3));
  EXPECT_EQ(c.value, 16);
}

TEST(BindGenericTest, BindRepeatingCallbackAsFunctor) {
  RepeatingCallback<int(int)> inc = [](int x) { return x + 1; };
  RepeatingCallback<int()> outer = BindRepeating(std::move(inc), 41);
  EXPECT_EQ(42, outer.Run());
  EXPECT_EQ(42, outer.Run());
}

} // namespace
} // namespace nei
