#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <neixx/task/message_loop/message_pump_io.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/synchronization/lock.h>
#include <neixx/threading/platform_thread.h>

namespace nei {
namespace {

constexpr ULONG_PTR kScheduleWorkCompletionKey = 1;
constexpr std::size_t kMaxCompletionBatch = 16;

int ComputeWaitTimeoutMs(const MessagePump::Delegate::NextWorkInfo& next_work_info,
                         const TimeTicks& delayed_run_time) {
  const TimeTicks now = !next_work_info.recent_now.is_null() ? next_work_info.recent_now
                                                             : TimeTicks::Now();
  if (delayed_run_time <= now) {
    return 0;
  }

  TimeDelta wait_delta = delayed_run_time - now;
  int64_t wait_ms = (wait_delta.InMicroseconds() + 999) / 1000;
  if (wait_ms < 0) {
    wait_ms = 0;
  }
  return static_cast<int>(wait_ms);
}

}  // namespace

class MessagePumpForIOState {
 public:
  struct WatchRecord {
    NativeIOHandle handle = NativeIOHandle{};
    MessagePumpForIO::Watcher* watcher = nullptr;
    MessagePumpForIO::FdWatchController::Mode mode = MessagePumpForIO::FdWatchController::Mode::READ;
  };

  MessagePumpForIOState() {
    iocp_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
  }

  ~MessagePumpForIOState() {
    if (iocp_ != nullptr) {
      (void)::CloseHandle(iocp_);
    }
  }

  int EnterRunLoopAndGetDepth(PlatformThread::PlatformThreadId current_thread_id) {
    AutoLock lock(state_lock_);
    if (run_thread_id_ == 0) {
      run_thread_id_ = current_thread_id;
    }
    ++run_depth_;
    return run_depth_;
  }

  void ExitRunLoop(int run_depth) {
    AutoLock lock(state_lock_);
    DCHECK(run_depth_ == run_depth);
    --run_depth_;
    const int quit_depth = quit_run_depth_.load(std::memory_order_acquire);
    if (quit_depth > run_depth_) {
      quit_run_depth_.store(0, std::memory_order_release);
    }
  }

  bool IsRunLoopActive(int run_depth) const {
    return quit_run_depth_.load(std::memory_order_acquire) < run_depth;
  }

  bool IsCurrentRunThread(PlatformThread::PlatformThreadId current_thread_id) const {
    AutoLock lock(state_lock_);
    if (run_thread_id_ == 0) {
      return true;
    }
    return run_thread_id_ == current_thread_id;
  }

  void RequestQuitInnermostRun() {
    {
      AutoLock lock(state_lock_);
      if (run_depth_ > 0) {
        quit_run_depth_.store(run_depth_, std::memory_order_release);
      }
      work_scheduled_ = true;
    }
    WakePump();
  }

  void ScheduleWork() {
    WakePump();
  }

  void ScheduleDelayedWork(const TimeTicks& delayed_run_time) {
    bool should_wake = false;
    {
      AutoLock lock(state_lock_);
      if (!has_delayed_run_time_ || delayed_run_time < delayed_run_time_) {
        delayed_run_time_ = delayed_run_time;
        has_delayed_run_time_ = true;
        should_wake = true;
      }
    }
    if (should_wake) {
      WakePump();
    }
  }

  void UpdateDelayedWorkFromDelegate(const MessagePump::Delegate::NextWorkInfo& next_work_info) {
    AutoLock lock(state_lock_);
    if (next_work_info.next_run_time == MessagePump::Delegate::NextWorkInfo::kNoScheduledRunTime) {
      return;
    }
    if (!has_delayed_run_time_ || next_work_info.next_run_time < delayed_run_time_) {
      delayed_run_time_ = next_work_info.next_run_time;
      has_delayed_run_time_ = true;
    }
  }

  bool GetDelayedRunTime(TimeTicks* delayed_run_time) const {
    AutoLock lock(state_lock_);
    if (!has_delayed_run_time_) {
      return false;
    }
    *delayed_run_time = delayed_run_time_;
    return true;
  }

  void ClearExpiredDelayedRunTime(TimeTicks now) {
    AutoLock lock(state_lock_);
    if (has_delayed_run_time_ && delayed_run_time_ <= now) {
      has_delayed_run_time_ = false;
    }
  }

  void DrainPendingWakeups(MessagePump::Delegate* delegate) {
    // 传入 should_run_task=false：仅排干 IOCP 中残留的唤醒包，不触发
    // delegate->DoWork()，防止 DoWork 内部 PostTask 再次不断进入死循环。
    while (DispatchOneBatch(delegate, 0, /*should_run_task=*/false)) {}
  }

  bool WaitAndDispatch(MessagePump::Delegate* delegate, int timeout_ms) {
    return DispatchOneBatch(delegate, timeout_ms);
  }

  bool RegisterWatch(MessagePumpForIO::FdWatchController* controller,
                     NativeIOHandle handle,
                     MessagePumpForIO::FdWatchController::Mode mode,
                     MessagePumpForIO::Watcher* watcher) {
    if (controller == nullptr || watcher == nullptr || iocp_ == nullptr) {
      return false;
    }

    const std::uint64_t watch_id = next_watch_id_.fetch_add(1, std::memory_order_relaxed);
    HANDLE native_handle = reinterpret_cast<HANDLE>(handle);
    if (::CreateIoCompletionPort(native_handle, iocp_, static_cast<ULONG_PTR>(watch_id), 0) == nullptr) {
      return false;
    }

    AutoLock lock(state_lock_);
    watches_[watch_id] = WatchRecord{handle, watcher, mode};
    return true;
  }

  void StopWatching(std::uint64_t watch_id) {
    if (watch_id == 0) {
      return;
    }
    AutoLock lock(state_lock_);
    watches_.erase(watch_id);
  }

 private:
  void WakePump() {
    if (iocp_ == nullptr) {
      return;
    }
    (void)::PostQueuedCompletionStatus(iocp_, 0, kScheduleWorkCompletionKey, nullptr);
  }

  bool DispatchOneBatch(MessagePump::Delegate* delegate, int timeout_ms,
                        bool should_run_task = true) {
    if (iocp_ == nullptr) {
      return false;
    }

    bool any_event = false;
    bool ran_work_wakeup = false;
    DWORD wait_timeout = static_cast<DWORD>(timeout_ms < 0 ? INFINITE : timeout_ms);
    for (std::size_t i = 0; i < kMaxCompletionBatch; ++i) {
      DWORD bytes_transferred = 0;
      ULONG_PTR completion_key = 0;
      OVERLAPPED* overlapped = nullptr;
      const BOOL ok = ::GetQueuedCompletionStatus(iocp_, &bytes_transferred, &completion_key,
                                                  &overlapped, wait_timeout);
      wait_timeout = 0;
      if (!ok) {
        const DWORD error = ::GetLastError();
        if (error == WAIT_TIMEOUT) {
          break;
        }
      }

      if (!ok && completion_key == 0 && overlapped == nullptr) {
        break;
      }

      if (completion_key == kScheduleWorkCompletionKey) {
        ran_work_wakeup = true;
        any_event = true;
        // 立即中断批量消费，优先将执行权交还给 DoWork，避免高吞吐 I/O
        // 场景下 Task 调度饥饿（Starvation）。
        break;
      }

      WatchRecord record;
      bool found = false;
      {
        AutoLock lock(state_lock_);
        auto it = watches_.find(static_cast<std::uint64_t>(completion_key));
        if (it != watches_.end()) {
          record = it->second;
          found = true;
        }
      }
      if (!found || record.watcher == nullptr) {
        continue;
      }

      const bool can_read = record.mode == MessagePumpForIO::FdWatchController::Mode::READ ||
                            record.mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE;
      const bool can_write = record.mode == MessagePumpForIO::FdWatchController::Mode::WRITE ||
                             record.mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE;
      if (can_read) {
        record.watcher->OnFileCanReadWithoutBlocking(record.handle);
      }
      if (can_write) {
        record.watcher->OnFileCanWriteWithoutBlocking(record.handle);
      }
      any_event = true;
    }

    if (ran_work_wakeup && delegate != nullptr && should_run_task) {
      (void)delegate->DoWork();
    }
    return any_event;
  }

  mutable Lock state_lock_;

  int run_depth_ = 0;
  std::atomic<int> quit_run_depth_{0};
  PlatformThread::PlatformThreadId run_thread_id_ = 0;

  bool work_scheduled_ = false;
  bool has_delayed_run_time_ = false;
  TimeTicks delayed_run_time_;

  HANDLE iocp_ = nullptr;
  std::atomic<std::uint64_t> next_watch_id_{2};
  std::unordered_map<std::uint64_t, WatchRecord> watches_;
};

MessagePumpForIO::FdWatchController::FdWatchController() = default;

MessagePumpForIO::FdWatchController::~FdWatchController() {
  StopWatching();
}

bool MessagePumpForIO::FdWatchController::StartWatching(MessagePumpForIO* pump,
                                                        NativeIOHandle handle,
                                                        MessagePumpForIO::FdWatchController::Mode mode,
                                                        MessagePumpForIO::Watcher* watcher) {
  if (pump == nullptr || watcher == nullptr || pump->impl_ == nullptr) {
    return false;
  }

  StopWatching();

  if (!pump->impl_->RegisterWatch(this, handle, mode, watcher)) {
    pump_ = nullptr;
    impl_.reset();
    handle_ = NativeIOHandle{};
    watcher_ = nullptr;
    mode_ = Mode::READ;
    watch_id_ = 0;
    return false;
  }

  pump_ = pump;
  impl_ = pump->impl_;
  handle_ = handle;
  watcher_ = watcher;
  mode_ = mode;
  return true;
}

void MessagePumpForIO::FdWatchController::StopWatching() {
  if (watch_id_ == 0) {
    pump_ = nullptr;
    impl_.reset();
    handle_ = NativeIOHandle{};
    watcher_ = nullptr;
    mode_ = Mode::READ;
    return;
  }

  if (impl_) {
    impl_->StopWatching(watch_id_);
  }

  pump_ = nullptr;
  impl_.reset();
  handle_ = NativeIOHandle{};
  watcher_ = nullptr;
  mode_ = Mode::READ;
  watch_id_ = 0;
}

bool MessagePumpForIO::FdWatchController::is_watching() const {
  return watch_id_ != 0;
}

MessagePumpForIO::MessagePumpForIO() : impl_(std::make_shared<MessagePumpForIOState>()) {}

MessagePumpForIO::~MessagePumpForIO() {
  Quit();
}

void MessagePumpForIO::Run(Delegate* delegate) {
  if (delegate == nullptr || impl_ == nullptr) {
    return;
  }

  const PlatformThread::PlatformThreadId current_thread_id = PlatformThread::CurrentId();
  DCHECK(impl_->IsCurrentRunThread(current_thread_id));

  const int run_depth = impl_->EnterRunLoopAndGetDepth(current_thread_id);

  while (impl_->IsRunLoopActive(run_depth)) {
    if (delegate->DoWork()) {
      continue;
    }

    Delegate::NextWorkInfo next_work_info;
    if (delegate->DoDelayedWork(&next_work_info)) {
      impl_->UpdateDelayedWorkFromDelegate(next_work_info);
      continue;
    }
    impl_->UpdateDelayedWorkFromDelegate(next_work_info);

    if (delegate->DoIdleWork()) {
      continue;
    }

    if (!impl_->IsRunLoopActive(run_depth)) {
      break;
    }

    impl_->DrainPendingWakeups(delegate);
    if (!impl_->IsRunLoopActive(run_depth)) {
      break;
    }

    TimeTicks delayed_run_time;
    if (!impl_->GetDelayedRunTime(&delayed_run_time)) {
      impl_->WaitAndDispatch(delegate, -1);
      continue;
    }

    const TimeTicks now = !next_work_info.recent_now.is_null() ? next_work_info.recent_now
                                                                : TimeTicks::Now();
    if (delayed_run_time <= now) {
      impl_->ClearExpiredDelayedRunTime(now);
      continue;
    }

    const int wait_ms = ComputeWaitTimeoutMs(next_work_info, delayed_run_time);
    if (!impl_->WaitAndDispatch(delegate, wait_ms)) {
      impl_->ClearExpiredDelayedRunTime(TimeTicks::Now());
    }
  }

  impl_->ExitRunLoop(run_depth);
}

void MessagePumpForIO::Quit() {
  if (impl_ != nullptr) {
    impl_->RequestQuitInnermostRun();
  }
}

void MessagePumpForIO::ScheduleWork() {
  if (impl_ != nullptr) {
    impl_->ScheduleWork();
  }
}

void MessagePumpForIO::ScheduleDelayedWork(const TimeTicks& delayed_run_time) {
  if (impl_ != nullptr) {
    impl_->ScheduleDelayedWork(delayed_run_time);
  }
}

}  // namespace nei

#endif  // defined(_WIN32)
