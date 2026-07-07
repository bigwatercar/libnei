#include <neixx/task/timer.h>

#include <utility>

#include <nei/debug/check.h>
#include <neixx/task/sequence_checker.h>
#include <neixx/functional/bind.h>
#include <neixx/memory/weak_ptr.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/thread_task_runner_handle.h>

namespace nei {

// =============================================================================
// OneShotTimer::Impl  --  单次定时器内部实现
// =============================================================================
//
// 线程模型：所有操作必须在绑定序列上执行（SequenceChecker 强制校验）。
// 无需锁  --  --  is_running_ 和 user_task_ 仅在单序列访问。
// 跨序列安全通过 PostDelayedTask + WeakPtr 机制保证：
//   - Stop() -> InvalidateWeakPtrs() -> 已投递的延迟任务检测到失效 -> 静默丢弃
//
// * WeakPtrFactory 必须是最后一个成员，确保析构时最先失效。
// =============================================================================

class OneShotTimer::Impl {
 public:
  Impl() = default;

  explicit Impl(scoped_refptr<TaskRunner> task_runner)
      : task_runner_(std::move(task_runner)) {}

  ~Impl() {
    // 析构时确保 WeakPtr 失效，防止在途回调访问已销毁对象。
    // 调用者应在析构前显式调用 Stop()；此处作为最后防线。
    weak_ptr_factory_.InvalidateWeakPtrs();
  }

  void Start(const Location& from_here, TimeDelta delay, OnceCallback<> task) {
    DCHECK(sequence_checker_.CalledOnValidSequence());

    if (!task_runner_) {
      task_runner_ = ThreadTaskRunnerHandle::Get();
    }
    // OneShotTimer::Start requires a TaskRunner.
    // Ensure the current thread has a SequenceManager,
    // or provide a TaskRunner via the constructor.
    DCHECK(task_runner_);

    // 先停止已有定时器（若有），确保之前投递的延迟任务失效
    Stop();

    posted_from_ = from_here;
    user_task_ = std::move(task);
    is_running_ = true;

    // * WeakPtr 安全投递：BindOnce 以 WeakPtr 为首个绑定参数时，
    //   自动校验其有效性（失效则静默跳过）。std::invoke 通过
    //   WeakPtr::operator->() 获取 Impl* 来调用成员函数。
    //   WeakPtrFactory 的惰性 flag 重建机制确保 Stop() 后新 WeakPtr 有效。
    task_runner_->PostDelayedTask(
        from_here,
        BindOnce(&Impl::OnTimerFired, weak_ptr_factory_.GetWeakPtr()),
        delay);
  }

  void Stop() {
    DCHECK(sequence_checker_.CalledOnValidSequence());

    // * 零损耗取消：InvalidateWeakPtrs() 使所有已投递的延迟任务在
    //   唤醒瞬间检测到 WeakPtr 失效，静默丢弃，无需额外同步开销。
    weak_ptr_factory_.InvalidateWeakPtrs();

    // * 立即释放用户闭包资源（智能指针、大块内存等）
    user_task_ = OnceCallback<>();
    is_running_ = false;
  }

  bool IsRunning() const {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    return is_running_;
  }

  const Location& posted_from() const { return posted_from_; }

 private:
  // ---------------------------------------------------------------------------
  // OnTimerFired  --  延迟任务到期回调
  //
  // 仅在 WeakPtr 有效时被 BindOnce 调用，即在 Stop() 或析构后不会被触发。
  // ---------------------------------------------------------------------------
  void OnTimerFired() {
    DCHECK(sequence_checker_.CalledOnValidSequence());

    // 额外的防御检查：理论上 BindOnce 的 WeakPtr 校验已保证可达性，
    // 但 is_running_ 可能因其他代码路径被置 false
    if (!is_running_)
      return;

    is_running_ = false;

    // * 锁外回调派发：将闭包转移到局部变量后执行，确保即使回调内部
    //   重入 Stop() / Start() 也不会访问已移动的 user_task_
    OnceCallback<> task = std::move(user_task_);
    // user_task_ 此时为空

    if (task) {
      std::move(task).Run();
    }
  }

  scoped_refptr<TaskRunner> task_runner_;
  SequenceChecker sequence_checker_;
  Location posted_from_;
  OnceCallback<> user_task_;
  bool is_running_ = false;

  // *** 必须为最后一个成员变量 ***
  // WeakPtrFactory 析构时自动 InvalidateWeakPtrs()，确保其失效先于
  // 其他成员的析构，防止悬空指针访问。
  WeakPtrFactory<Impl> weak_ptr_factory_{this};
};

// =============================================================================
// RepeatingTimer::Impl  --  周期定时器内部实现
// =============================================================================
//
// 与 OneShotTimer 共享相同的线程模型（单序列 + SequenceChecker）。
//
// * 自毁灭防御（Re-entrancy Guard）：
//   OnTimerFired() 执行用户回调后，必须重新检查 is_running_。
//   因为用户可能在回调中调用了 Stop()，若盲目 ScheduleNextTick()
//   将导致停不下来的僵尸心跳。
//
// * WeakPtrFactory 必须是最后一个成员。
// =============================================================================

class RepeatingTimer::Impl {
 public:
  Impl() = default;

  explicit Impl(scoped_refptr<TaskRunner> task_runner)
      : task_runner_(std::move(task_runner)) {}

  ~Impl() {
    weak_ptr_factory_.InvalidateWeakPtrs();
  }

  void Start(const Location& from_here, TimeDelta delay, RepeatingCallback<> task) {
    DCHECK(sequence_checker_.CalledOnValidSequence());

    if (!task_runner_) {
      task_runner_ = ThreadTaskRunnerHandle::Get();
    }
    // RepeatingTimer::Start requires a TaskRunner.
    // Ensure the current thread has a SequenceManager,
    // or provide a TaskRunner via the constructor.
    DCHECK(task_runner_);

    Stop();

    posted_from_ = from_here;
    user_task_ = std::move(task);
    delay_ = delay;
    is_running_ = true;

    ScheduleNextTick();
  }

  void Stop() {
    DCHECK(sequence_checker_.CalledOnValidSequence());

    weak_ptr_factory_.InvalidateWeakPtrs();
    user_task_ = RepeatingCallback<>();
    is_running_ = false;
  }

  bool IsRunning() const {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    return is_running_;
  }

  const Location& posted_from() const { return posted_from_; }

 private:
  // ---------------------------------------------------------------------------
  // ScheduleNextTick  --  投递下一次定时任务
  // ---------------------------------------------------------------------------
  void ScheduleNextTick() {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    task_runner_->PostDelayedTask(
        posted_from_,
        BindOnce(&Impl::OnTimerFired, weak_ptr_factory_.GetWeakPtr()),
        delay_);
  }

  // ---------------------------------------------------------------------------
  // OnTimerFired  --  周期到期回调
  //
  // 仅在 WeakPtr 有效时被调用。
  //
  // * 执行顺序（关键）：
  //   1. 检查 is_running_（防御）
  //   2. 执行用户回调
  //   3. * 重新检查 is_running_（自毁灭防御）
  //   4. 若仍在运行，安排下一次 tick
  // ---------------------------------------------------------------------------
  void OnTimerFired() {
    DCHECK(sequence_checker_.CalledOnValidSequence());

    if (!is_running_)
      return;

    // * 执行用户回调（无锁上下文）
    // 先拷贝到局部变量再执行，防止回调内调用 Stop() 导致 user_task_
    // 被销毁而 Run() 仍在访问闭包内存（use-after-destroy / SEH）。
    if (user_task_) {
      RepeatingCallback<> task = user_task_;  // 拷贝 -> 引用计数 +1（或 SBO copy）
      task.Run();  // 安全：即使 Stop() 销毁 user_task_，task 仍持有引用
    }

    // *** 自毁灭防御（Re-entrancy Guard）***
    // 用户可能在回调中调用了 Stop()，必须重新检查。
    // 若此时仍盲目调用 ScheduleNextTick()，将导致：
    //   - 定时器表面已"停止"，但后台仍在投递新任务
    //   - WeakPtr 已失效，每个新任务在唤醒瞬间被静默丢弃
    //   - 等同于僵尸心跳  --  --  浪费 CPU 和内存
    if (!is_running_)
      return;

    ScheduleNextTick();
  }

  scoped_refptr<TaskRunner> task_runner_;
  SequenceChecker sequence_checker_;
  Location posted_from_;
  RepeatingCallback<> user_task_;
  TimeDelta delay_;
  bool is_running_ = false;

  // *** 必须为最后一个成员变量 ***
  WeakPtrFactory<Impl> weak_ptr_factory_{this};
};

// =============================================================================
// OneShotTimer  --  公开接口实现
// =============================================================================

OneShotTimer::OneShotTimer() : impl_(new Impl()) {}

OneShotTimer::OneShotTimer(scoped_refptr<TaskRunner> task_runner)
    : impl_(new Impl(std::move(task_runner))) {}

OneShotTimer::~OneShotTimer() = default;

void OneShotTimer::Start(const Location& from_here,
                         TimeDelta delay,
                         OnceCallback<> task) {
  impl_->Start(from_here, delay, std::move(task));
}

void OneShotTimer::Stop() {
  impl_->Stop();
}

bool OneShotTimer::IsRunning() const {
  return impl_->IsRunning();
}

const Location& OneShotTimer::posted_from() const {
  return impl_->posted_from();
}

// =============================================================================
// RepeatingTimer  --  公开接口实现
// =============================================================================

RepeatingTimer::RepeatingTimer() : impl_(new Impl()) {}

RepeatingTimer::RepeatingTimer(scoped_refptr<TaskRunner> task_runner)
    : impl_(new Impl(std::move(task_runner))) {}

RepeatingTimer::~RepeatingTimer() = default;

void RepeatingTimer::Start(const Location& from_here,
                           TimeDelta delay,
                           RepeatingCallback<> task) {
  impl_->Start(from_here, delay, std::move(task));
}

void RepeatingTimer::Stop() {
  impl_->Stop();
}

bool RepeatingTimer::IsRunning() const {
  return impl_->IsRunning();
}

const Location& RepeatingTimer::posted_from() const {
  return impl_->posted_from();
}

}  // namespace nei
