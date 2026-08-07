#pragma once

#ifndef NEIXX_TASK_TIMER_H_
#define NEIXX_TASK_TIMER_H_

// =============================================================================
// OneShotTimer / RepeatingTimer  --  高精度定时器 (Chromium-style)
// =============================================================================
//
// 基于 PostDelayedTask + WeakPtr 实现，零额外线程开销。
//
// * 序列安全：Start()、Stop() 和定时器回调均在绑定的 TaskRunner 序列
//   上执行，内部通过 SequenceChecker 强制执行运行期校验。
//
// * 零损耗取消 (OneShotTimer): Stop() 调用 InvalidateWeakPtrs()，
//   已投递的延迟任务在唤醒瞬间检测到 WeakPtr 失效，静默丢弃，无需
//   任何额外的同步开销。
//
// * 自毁灭防御 (RepeatingTimer): 底层回调执行用户任务后，重新检查
//   is_running_ 标志。若用户在回调中调用了 Stop()，则不安排下一次
//   tick，彻底杜绝僵尸心跳。
//
// 使用范式:
//
//   // 单次定时器
//   OneShotTimer timer;
//   timer.Start(FROM_HERE, TimeDelta::FromSeconds(5),
//               BindOnce(&MyClass::OnTimeout, weak_factory_.GetWeakPtr()));
//   // 可提前取消
//   timer.Stop();
//
//   // 周期定时器
//   RepeatingTimer timer;
//   timer.Start(FROM_HERE, TimeDelta::FromMilliseconds(100),
//               BindRepeating(&MyClass::OnTick, weak_factory_.GetWeakPtr()));
//   // 业务回调内部可安全调用 timer.Stop() 终止心跳
//
// * PIMPL 保证：公开头文件不暴露 std::mutex、SequenceChecker 或
//   底层计时器实现细节。
// =============================================================================

#include <memory>

#include <nei/build/nei_export.h>
#include <nei/build/compiler_specific.h>
#include <neixx/common/location.h>
#include <neixx/common/time.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/task/task_runner.h>

namespace nei {

// =============================================================================
// OneShotTimer  --  单次高精度定时器
// =============================================================================
class NEI_API OneShotTimer final {
public:
  // Creates a timer. The TaskRunner is captured from the current thread
  // at Start() time via ThreadTaskRunnerHandle::Get().
  OneShotTimer();

  // Creates a timer that posts delayed tasks to |task_runner|.
  explicit OneShotTimer(scoped_refptr<SequencedTaskRunner> task_runner);

  ~OneShotTimer();

  OneShotTimer(const OneShotTimer &) = delete;
  OneShotTimer &operator=(const OneShotTimer &) = delete;

  // Starts the timer. |task| will be run on the bound sequence after |delay|.
  // If the timer is already running, it is first stopped.
  // Must be called on the bound sequence.
  void Start(const Location &from_here, TimeDelta delay, OnceCallback<void()> task);

  // Stops the timer. Invalidates outstanding WeakPtrs so any queued delayed
  // task is silently dropped, and immediately resets the user closure.
  // Must be called on the bound sequence.
  void Stop();

  // Returns true if the timer is running (Start() called, not yet fired/stopped).
  bool IsRunning() const;

  // Returns the location from which Start() was last called.
  const Location &posted_from() const;

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

// =============================================================================
// RepeatingTimer  --  周期高精度定时器
// =============================================================================
class NEI_API RepeatingTimer final {
public:
  RepeatingTimer();
  explicit RepeatingTimer(scoped_refptr<SequencedTaskRunner> task_runner);
  ~RepeatingTimer();

  RepeatingTimer(const RepeatingTimer &) = delete;
  RepeatingTimer &operator=(const RepeatingTimer &) = delete;

  // Starts the timer. |task| will be run repeatedly every |delay| on the
  // bound sequence until Stop() is called.
  // Must be called on the bound sequence.
  void Start(const Location &from_here, TimeDelta delay, RepeatingCallback<void()> task);

  // Stops the timer. Invalidates outstanding WeakPtrs and resets the user task.
  // Safe to call from within the user callback (re-entrancy safe).
  // Must be called on the bound sequence.
  void Stop();

  bool IsRunning() const;

  const Location &posted_from() const;

private:
  class Impl;
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END()
};

} // namespace nei

#endif // NEIXX_TASK_TIMER_H_
