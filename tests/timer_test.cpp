#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include <neixx/common/time.h>
#include <neixx/functional/bind.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/sequence_manager.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/timer.h>

namespace nei {
namespace {

// =============================================================================
// TimerTest — 辅助函数
//
// 每个测试自行创建 SequenceManager，避免线程绑定跨测试泄漏。
// RunPumpWithTimeout 投递延迟 Quit 后驱动 pump，
// RunPumpSimple 投递立即 Quit 后驱动 pump。
// =============================================================================

class ScopedSequenceManager {
 public:
  ScopedSequenceManager() {
    manager_ = std::make_unique<SequenceManager>(
        std::make_unique<MessagePumpDefault>());
    runner_ = manager_->CreateTaskRunner();
  }

  ~ScopedSequenceManager() {
    // Ensure pump is not running, then shutdown to unbind thread
    if (manager_) {
      manager_->Shutdown();
    }
  }

  SequenceManager* manager() { return manager_.get(); }
  TaskRunner* runner() { return runner_.get(); }

  // 驱动 pump 直到 manager_->Quit() 被调用
  void RunPump() { manager_->Run(); }

  // 投递一个延迟 Quit 后驱动 pump
  void RunPumpWithTimeout(int delay_ms = 200) {
    runner_->PostDelayedTask(
        FROM_HERE,
        BindOnce([](SequenceManager* mgr) { mgr->Quit(); }, manager_.get()),
        TimeDelta::FromMilliseconds(delay_ms));
    manager_->Run();
  }

 private:
  std::unique_ptr<SequenceManager> manager_;
  scoped_refptr<TaskRunner> runner_;
};

// =============================================================================
// OneShotTimer
// =============================================================================

TEST(TimerTest, OneShotTimerFiresOnce) {
  ScopedSequenceManager mgr;
  std::atomic<bool> fired{false};
  OneShotTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(10),
                BindOnce([&] { fired.store(true); }));
  });

  mgr.RunPumpWithTimeout();
  EXPECT_TRUE(fired.load());
}

TEST(TimerTest, OneShotTimerStopPreventsFire) {
  ScopedSequenceManager mgr;
  std::atomic<bool> fired{false};
  OneShotTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(500),
                BindOnce([&] { fired.store(true); }));
    timer.Stop();
  });

  mgr.RunPumpWithTimeout(100);
  EXPECT_FALSE(fired.load());
}

TEST(TimerTest, OneShotTimerIsRunningReflectsState) {
  ScopedSequenceManager mgr;
  OneShotTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromSeconds(10), BindOnce([] {}));
    EXPECT_TRUE(timer.IsRunning());
    timer.Stop();
    EXPECT_FALSE(timer.IsRunning());
    mgr.manager()->Quit();
  });

  mgr.RunPump();
}

TEST(TimerTest, OneShotTimerRestartCancelsPrevious) {
  ScopedSequenceManager mgr;
  std::atomic<int> count{0};
  OneShotTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(500),
                BindOnce([&] { count.fetch_add(1); }));
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(10),
                BindOnce([&] { count.fetch_add(1); }));
  });

  mgr.RunPumpWithTimeout(100);
  EXPECT_EQ(count.load(), 1);
}

TEST(TimerTest, OneShotTimerPostedFromRecordsLocation) {
  ScopedSequenceManager mgr;
  OneShotTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(10), BindOnce([&] {
      EXPECT_FALSE(timer.posted_from().is_null());
      mgr.manager()->Quit();
    }));
  });

  mgr.RunPump();
}

// ---------------------------------------------------------------------------
// ★ 析构安全拦截
// ---------------------------------------------------------------------------

TEST(TimerTest, OneShotTimerDestructionPreventsCallback) {
  std::atomic<bool> fired{false};

  {
    ScopedSequenceManager mgr;
    OneShotTimer timer;
    mgr.runner()->PostTask(FROM_HERE, [&] {
      timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(10),
                  BindOnce([&] { fired.store(true); }));
    });
    // 驱动 pump 确保 Start() 已执行
    mgr.runner()->PostTask(FROM_HERE, [&mgr] { mgr.manager()->Quit(); });
    mgr.RunPump();
    // timer 在此析构 → InvalidateWeakPtrs()
  }

  // 创建新的 manager 排干延迟任务
  ScopedSequenceManager mgr2;
  mgr2.RunPumpWithTimeout(100);
  EXPECT_FALSE(fired.load());
}

// =============================================================================
// RepeatingTimer
// =============================================================================

TEST(TimerTest, RepeatingTimerFiresMultipleTimes) {
  ScopedSequenceManager mgr;
  std::atomic<int> count{0};
  RepeatingTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(5),
                BindRepeating([&] {
                  if (count.fetch_add(1) >= 3) {
                    timer.Stop();
                    mgr.manager()->Quit();
                  }
                }));
  });

  mgr.RunPump();
  EXPECT_GE(count.load(), 4);
}

// ---------------------------------------------------------------------------
// ★ 自毁灭重入攻击
// ---------------------------------------------------------------------------

TEST(TimerTest, RepeatingTimerStopFromWithinCallback) {
  ScopedSequenceManager mgr;
  std::atomic<int> count{0};
  RepeatingTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(5),
                BindRepeating([&] {
                  count.fetch_add(1);
                  timer.Stop();
                  mgr.runner()->PostDelayedTask(
                      FROM_HERE,
                      BindOnce([](SequenceManager* m) { m->Quit(); },
                               mgr.manager()),
                      TimeDelta::FromMilliseconds(50));
                }));
  });

  mgr.RunPump();
  EXPECT_EQ(count.load(), 1);
}

TEST(TimerTest, RepeatingTimerIsRunningReflectsState) {
  ScopedSequenceManager mgr;
  RepeatingTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromSeconds(10), BindRepeating([] {}));
    EXPECT_TRUE(timer.IsRunning());
    timer.Stop();
    EXPECT_FALSE(timer.IsRunning());
    mgr.manager()->Quit();
  });

  mgr.RunPump();
}

TEST(TimerTest, RepeatingTimerRestartResetsTimer) {
  ScopedSequenceManager mgr;
  std::atomic<int> count{0};
  RepeatingTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromSeconds(10),
                BindRepeating([&] { count.fetch_add(1); }));
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(5),
                BindRepeating([&] {
                  if (count.fetch_add(1) >= 2) {
                    timer.Stop();
                    mgr.manager()->Quit();
                  }
                }));
  });

  mgr.RunPump();
  EXPECT_GE(count.load(), 3);
}

TEST(TimerTest, RepeatingTimerStopPreventsFurtherFires) {
  ScopedSequenceManager mgr;
  std::atomic<int> count{0};
  RepeatingTimer timer;

  mgr.runner()->PostTask(FROM_HERE, [&count, runner = mgr.runner(), mgr_ptr = mgr.manager()] {
    auto t = std::make_unique<RepeatingTimer>();
    t->Start(FROM_HERE, TimeDelta::FromMilliseconds(5),
             BindRepeating([&count] { count.fetch_add(1); }));

    runner->PostDelayedTask(
        FROM_HERE,
        [t = std::move(t), runner, mgr_ptr] {
          runner->PostDelayedTask(
              FROM_HERE,
              BindOnce([](SequenceManager* m) { m->Quit(); }, mgr_ptr),
              TimeDelta::FromMilliseconds(60));
        },
        TimeDelta::FromMilliseconds(30));
  });

  mgr.RunPump();
  EXPECT_GE(count.load(), 1);
}

// =============================================================================
// 显式 TaskRunner 绑定
// =============================================================================

TEST(TimerTest, ExplicitTaskRunnerBinding) {
  ScopedSequenceManager mgr;
  std::atomic<bool> fired{false};
  OneShotTimer timer(scoped_refptr<TaskRunner>(mgr.runner()));

  mgr.runner()->PostTask(FROM_HERE, [&] {
    timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(5),
                BindOnce([&] { fired.store(true); }));
  });

  mgr.RunPumpWithTimeout(100);
  EXPECT_TRUE(fired.load());
}

}  // namespace
}  // namespace nei
