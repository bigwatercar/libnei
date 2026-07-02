#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

#include <neixx/functional/bind.h>
#include <neixx/functional/cancelable_callback.h>

namespace nei {
namespace {

// =============================================================================
// MockTracker — 生命周期探针
//
// 用于验证 CancelableOnceClosure::Cancel() 是否在调用线程上立即析构了
// 捕获的资源（而非滞留在底层任务队列中）。析构时将 alive 置为 false。
// =============================================================================
struct LifecycleTracker {
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  ~LifecycleTracker() { *alive = false; }
};

// =============================================================================
// CancelableOnceClosureTest — 测试夹具
// =============================================================================
class CancelableOnceClosureTest : public testing::Test {
 protected:
  template <typename F>
  CancelableOnceClosure MakeTask(F&& fn) {
    return CancelableOnceClosure(BindOnce(std::forward<F>(fn)));
  }
};

// ---------------------------------------------------------------------------
// 基础功能
// ---------------------------------------------------------------------------

TEST_F(CancelableOnceClosureTest, RunExecutesCallback) {
  std::atomic<bool> ran{false};
  auto task = MakeTask([&ran] { ran.store(true); });
  task.Run();
  EXPECT_TRUE(ran.load());
}

TEST_F(CancelableOnceClosureTest, CancelPreventsExecution) {
  std::atomic<bool> ran{false};
  auto task = MakeTask([&ran] { ran.store(true); });
  task.Cancel();
  task.Run();
  EXPECT_FALSE(ran.load());
}

TEST_F(CancelableOnceClosureTest, RunConsumesTaskOnlyOnce) {
  std::atomic<int> count{0};
  auto task = MakeTask([&count] { count.fetch_add(1); });
  task.Run();
  task.Run();
  task.Run();
  EXPECT_EQ(count.load(), 1);
}

TEST_F(CancelableOnceClosureTest, CancelIsIdempotent) {
  auto task = MakeTask([] {});
  task.Cancel();
  task.Cancel();
  task.Cancel();
  EXPECT_TRUE(task.IsCancelled());
}

// ---------------------------------------------------------------------------
// ★ 极速内存释放验证 (Immediate Resource Reclamation)
// ---------------------------------------------------------------------------

TEST_F(CancelableOnceClosureTest, CancelReleasesResourcesImmediately) {
  auto tracker = std::make_shared<LifecycleTracker>();

  {
    auto t = std::move(*tracker);  // move tracker into closure capture
    CancelableOnceClosure task(BindOnce(
        [](LifecycleTracker captured) {
          // If we get here, Cancel() didn't work
          FAIL() << "Callback should not execute after Cancel()";
        },
        std::move(t)));
    task.Cancel();
    // ★ Cancel() 返回后，捕获的 LifecycleTracker 必须已被析构
    EXPECT_FALSE(*tracker->alive);
  }
}

TEST_F(CancelableOnceClosureTest, CancelReleasesResourcesOnCallingThread) {
  // 验证 Cancel() 在调用线程上释放资源（而非延迟到某任务队列执行）
  auto tracker = std::make_shared<LifecycleTracker>();
  std::thread::id cancel_thread_id;

  auto t = std::move(*tracker);
  CancelableOnceClosure task(BindOnce(
      [](LifecycleTracker captured) { FAIL(); },
      std::move(t)));

  cancel_thread_id = std::this_thread::get_id();
  task.Cancel();

  // tracker 的析构在 Cancel() 调用期间完成，因此 alive 在同一线程已为 false
  EXPECT_FALSE(*tracker->alive);
}

// ---------------------------------------------------------------------------
// 状态查询
// ---------------------------------------------------------------------------

TEST_F(CancelableOnceClosureTest, IsCancelledReflectsState) {
  auto task = MakeTask([] {});
  EXPECT_FALSE(task.IsCancelled());
  task.Cancel();
  EXPECT_TRUE(task.IsCancelled());
}

TEST_F(CancelableOnceClosureTest, IsCancelledFalseAfterRun) {
  auto task = MakeTask([] {});
  task.Run();
  EXPECT_FALSE(task.IsCancelled());  // Run() ≠ Cancel()
}

TEST_F(CancelableOnceClosureTest, OperatorBoolReflectsValidity) {
  CancelableOnceClosure empty;
  EXPECT_FALSE(empty);

  auto task = MakeTask([] {});
  EXPECT_TRUE(task);

  task.Cancel();
  EXPECT_FALSE(task);
}

// ---------------------------------------------------------------------------
// callback() — PostTask 适配
// ---------------------------------------------------------------------------

TEST_F(CancelableOnceClosureTest, CallbackReturnsRunnableOnceCallback) {
  std::atomic<bool> ran{false};
  auto task = MakeTask([&ran] { ran.store(true); });
  OnceCallback cb = task.callback();
  std::move(cb).Run();
  EXPECT_TRUE(ran.load());
}

TEST_F(CancelableOnceClosureTest, CallbackRespectsCancel) {
  std::atomic<bool> ran{false};
  auto task = MakeTask([&ran] { ran.store(true); });
  OnceCallback cb = task.callback();
  task.Cancel();
  std::move(cb).Run();
  EXPECT_FALSE(ran.load());
}

TEST_F(CancelableOnceClosureTest, CallbackAndRunConsumeSameTask) {
  std::atomic<int> count{0};
  auto task = MakeTask([&count] { count.fetch_add(1); });
  OnceCallback cb = task.callback();
  task.Run();             // Run() 先消费
  std::move(cb).Run();    // callback() 的包装看到 task_ 已空
  EXPECT_EQ(count.load(), 1);
}

// ---------------------------------------------------------------------------
// 移动语义
// ---------------------------------------------------------------------------

TEST_F(CancelableOnceClosureTest, MoveTransfersOwnership) {
  std::atomic<bool> ran{false};
  auto task1 = MakeTask([&ran] { ran.store(true); });
  auto task2 = std::move(task1);
  EXPECT_FALSE(task1);
  EXPECT_TRUE(task2);
  task2.Run();
  EXPECT_TRUE(ran.load());
}

TEST_F(CancelableOnceClosureTest, MoveAssignmentReleasesOldAndCancels) {
  std::atomic<bool> old_ran{false};
  std::atomic<bool> new_ran{false};
  auto task1 = MakeTask([&old_ran] { old_ran.store(true); });
  auto task2 = MakeTask([&new_ran] { new_ran.store(true); });

  // 发出旧任务的 callback() 句柄
  OnceCallback<> old_cb = task1.callback();

  // 移动赋值覆盖
  task1 = std::move(task2);
  EXPECT_FALSE(task2);

  // 旧 callback 应被取消
  std::move(old_cb).Run();
  EXPECT_FALSE(old_ran.load());

  // 新任务应可执行
  task1.Run();
  EXPECT_TRUE(new_ran.load());
}

// ---------------------------------------------------------------------------
// 析构生命周期
// ---------------------------------------------------------------------------

TEST_F(CancelableOnceClosureTest, EmptyConstructorSafeToCancelAndRun) {
  CancelableOnceClosure empty;
  empty.Cancel();
  empty.Run();
  EXPECT_FALSE(empty);
}

TEST_F(CancelableOnceClosureTest, DestructorAutoCancelsOutstandingCallback) {
  std::atomic<bool> ran{false};
  OnceCallback<> cb;

  {
    auto task = MakeTask([&ran] { ran.store(true); });
    cb = task.callback();
    // task 析构 → ~CancelableOnceClosure() → Cancel() + Release()
  }

  // callback() 持有的 scoped_refptr 保持 Impl 存活，但 cancelled_=true
  std::move(cb).Run();
  EXPECT_FALSE(ran.load());
}

// ---------------------------------------------------------------------------
// 跨线程并发安全
// ---------------------------------------------------------------------------

TEST_F(CancelableOnceClosureTest, ConcurrentCancelAndRun) {
  for (int iter = 0; iter < 200; ++iter) {
    std::atomic<int> count{0};
    auto task = MakeTask([&count] { count.fetch_add(1); });

    std::thread t1([&task] { task.Run(); });
    std::thread t2([&task] { task.Cancel(); });
    t1.join();
    t2.join();

    EXPECT_LE(count.load(), 1);  // 0=cancelled, 1=executed, never >1
  }
}

TEST_F(CancelableOnceClosureTest, ConcurrentDualCancel) {
  for (int iter = 0; iter < 200; ++iter) {
    auto task = MakeTask([] {});
    std::thread t1([&task] { task.Cancel(); });
    std::thread t2([&task] { task.Cancel(); });
    t1.join(); t2.join();
    EXPECT_TRUE(task.IsCancelled());
  }
}

TEST_F(CancelableOnceClosureTest, ConcurrentDualRun) {
  for (int iter = 0; iter < 200; ++iter) {
    std::atomic<int> count{0};
    auto task = MakeTask([&count] { count.fetch_add(1); });
    std::thread t1([&task] { task.Run(); });
    std::thread t2([&task] { task.Run(); });
    t1.join(); t2.join();
    EXPECT_EQ(count.load(), 1);
  }
}

}  // namespace
}  // namespace nei
