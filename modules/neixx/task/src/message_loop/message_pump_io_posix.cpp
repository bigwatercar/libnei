#if !defined(_WIN32)

#include <neixx/task/message_loop/message_pump_io.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <nei/debug/check.h>
#include <neixx/synchronization/lock.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread_local_storage.h>
#include <neixx/trace_event/trace_event.h>

namespace nei {
namespace {

ThreadLocalStorage::Slot& GetCurrentPumpSlot() {
  static ThreadLocalStorage::Slot slot;
  static std::once_flag once;
  std::call_once(once, []() {
    (void)slot.Initialize();
  });
  return slot;
}

constexpr std::size_t kMaxEpollEvents = 16;

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
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
      std::fprintf(stderr, "[MessagePumpForIO] epoll_create1 failed: %s\n",
                   std::strerror(errno));
      return;
    }

    event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
      std::fprintf(stderr, "[MessagePumpForIO] eventfd failed: %s\n",
                   std::strerror(errno));
      (void)close(epoll_fd_);
      epoll_fd_ = -1;
      return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    // Use the eventfd's own fd as the epoll token so the dispatch loop can
    // distinguish wakeup events from regular I/O by comparing ev.data.fd
    // against event_fd_.  This avoids the union-aliasing risk of using a
    // small magic constant (e.g. 1) in ev.data.u64, which would collide
    // with a real user fd that happens to have the same numeric value.
    ev.data.fd = event_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) != 0) {
      std::fprintf(stderr, "[MessagePumpForIO] epoll_ctl ADD wakefd failed: %s\n",
                   std::strerror(errno));
      (void)close(event_fd_);
      (void)close(epoll_fd_);
      event_fd_ = -1;
      epoll_fd_ = -1;
    }
  }

  ~MessagePumpForIOState() {
    if (event_fd_ >= 0) {
      (void)close(event_fd_);
    }
    if (epoll_fd_ >= 0) {
      (void)close(epoll_fd_);
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
    TRACE_EVENT0("nei.message_pump", "DrainPendingWakeups");
    // 传入 should_run_task=false：仅排干 OS 层的 eventfd 信号，不触发
    // delegate->DoWork()，防止 DoWork 内部 PostTask 再次写 eventfd 导致死循环。
    while (DispatchOneBatch(delegate, 0, /*should_run_task=*/false)) {}
  }

  bool WaitAndDispatch(MessagePump::Delegate* delegate, int timeout_ms) {
    TRACE_EVENT0("nei.message_pump", "WaitAndDispatch");
    return DispatchOneBatch(delegate, timeout_ms);
  }

  bool RegisterWatch(MessagePumpForIO::FdWatchController* controller,
                     NativeIOHandle handle,
                     MessagePumpForIO::FdWatchController::Mode mode,
                     MessagePumpForIO::Watcher* watcher) {
    if (controller == nullptr || watcher == nullptr || epoll_fd_ < 0 || event_fd_ < 0) {
      return false;
    }

    const std::uint64_t watch_id = next_watch_id_.fetch_add(1, std::memory_order_relaxed);

    // Build the desired event mask for this watch.
    std::uint32_t desired_events = EPOLLERR | EPOLLHUP;
    if (mode == MessagePumpForIO::FdWatchController::Mode::READ || mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE) {
      desired_events |= EPOLLIN;
    }
    if (mode == MessagePumpForIO::FdWatchController::Mode::WRITE || mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE) {
      desired_events |= EPOLLOUT;
    }

    int epoll_op = EPOLL_CTL_ADD;
    {
      AutoLock lock(state_lock_);
      // If another watch already exists for this fd, use EPOLL_CTL_MOD to
      // merge the new flags instead of ADD (which would fail with EEXIST).
      for (const auto& kv : watches_) {
        if (kv.second.handle == handle) {
          epoll_op = EPOLL_CTL_MOD;
          // Merge the existing watch's events with our desired events.
          std::uint32_t existing = EPOLLERR | EPOLLHUP;
          if (kv.second.mode == MessagePumpForIO::FdWatchController::Mode::READ ||
              kv.second.mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE) {
            existing |= EPOLLIN;
          }
          if (kv.second.mode == MessagePumpForIO::FdWatchController::Mode::WRITE ||
              kv.second.mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE) {
            existing |= EPOLLOUT;
          }
          desired_events |= existing;
          break;
        }
      }
      watches_[watch_id] = WatchRecord{handle, watcher, mode};
    }

    epoll_event ev{};
    ev.events = desired_events;
    // Use fd as the epoll token instead of watch_id so that DispatchOneBatch
    // can look up ALL watchers for this fd (read + write) by iterating the
    // watches_ map, rather than depending on a single watch_id that would
    // only reach one of them.
    ev.data.fd = static_cast<int>(handle);

    if (epoll_ctl(epoll_fd_, epoll_op, static_cast<int>(handle), &ev) != 0) {
      // If MOD fails because the fd was already deleted (race with StopWatching
      // on another controller), try ADD as a fallback.
      if (epoll_op == EPOLL_CTL_MOD && errno == ENOENT) {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, static_cast<int>(handle), &ev) != 0) {
          AutoLock lock(state_lock_);
          watches_.erase(watch_id);
          return false;
        }
      } else {
        AutoLock lock(state_lock_);
        watches_.erase(watch_id);
        return false;
      }
    }

    controller->pump_ = nullptr;  // set by owner after success
    controller->handle_ = handle;
    controller->watcher_ = watcher;
    controller->mode_ = mode;
    controller->watch_id_ = watch_id;
    return true;
  }

  void StopWatching(std::uint64_t watch_id, NativeIOHandle handle) {
    if (watch_id == 0) {
      return;
    }

    AutoLock lock(state_lock_);
    watches_.erase(watch_id);

    // Compute the remaining event mask for this fd.  If other watches still
    // exist for the same handle, use EPOLL_CTL_MOD to keep those flags alive
    // instead of EPOLL_CTL_DEL which would drop all watches for this fd.
    std::uint32_t remaining_events = 0;
    bool has_other = false;
    for (const auto& kv : watches_) {
      if (kv.second.handle == handle) {
        has_other = true;
        remaining_events |= EPOLLERR | EPOLLHUP;
        if (kv.second.mode == MessagePumpForIO::FdWatchController::Mode::READ ||
            kv.second.mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE) {
          remaining_events |= EPOLLIN;
        }
        if (kv.second.mode == MessagePumpForIO::FdWatchController::Mode::WRITE ||
            kv.second.mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE) {
          remaining_events |= EPOLLOUT;
        }
      }
    }

    if (epoll_fd_ >= 0) {
      if (has_other && remaining_events != 0) {
        // Other watches still exist — modify instead of delete.
        // Use fd as the epoll token (consistent with RegisterWatch).
        epoll_event ev{};
        ev.events = remaining_events;
        ev.data.fd = static_cast<int>(handle);
        (void)epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, static_cast<int>(handle), &ev);
      } else {
        (void)epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, static_cast<int>(handle), nullptr);
      }
    }
  }

 private:
  void WakePump() {
    TRACE_EVENT_INSTANT("nei.message_pump", "WakePump");
    if (event_fd_ < 0) {
      return;
    }
    const std::uint64_t value = 1;
    const ssize_t result = write(event_fd_, &value, sizeof(value));
    (void)result;
  }

  bool DrainWakeEvent() {
    if (event_fd_ < 0) {
      return false;
    }

    bool drained = false;
    for (;;) {
      std::uint64_t value = 0;
      const ssize_t read_result = read(event_fd_, &value, sizeof(value));
      if (read_result == static_cast<ssize_t>(sizeof(value))) {
        drained = true;
        continue;
      }
      if (read_result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      break;
    }
    return drained;
  }

  bool DispatchOneBatch(MessagePump::Delegate* delegate, int timeout_ms,
                        bool should_run_task = true) {
    if (epoll_fd_ < 0) {
      return false;
    }

    std::array<epoll_event, kMaxEpollEvents> events{};
    const int event_count = epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), timeout_ms);
    if (event_count < 0) {
      if (errno == EINTR) {
        return false;
      }
      return false;
    }

    bool any_event = false;
    bool ran_work_wakeup = false;
    for (int i = 0; i < event_count; ++i) {
      const epoll_event& event = events[static_cast<std::size_t>(i)];
      if (event.data.fd == event_fd_) {
        (void)DrainWakeEvent();
        TRACE_EVENT_INSTANT("nei.message_pump", "WakeEventDrained");
        ran_work_wakeup = true;
        any_event = true;
        continue;
      }

      // Extract the triggered fd from the epoll token (set by RegisterWatch).
      int triggered_fd = event.data.fd;

      // Collect all watchers for this fd under the lock.  A single fd may
      // have up to two watchers (one READ, one WRITE).  After collecting,
      // we release the lock and invoke callbacks so that re-entrant
      // StartWatching / StopWatching inside callbacks cannot deadlock.
      std::vector<WatchRecord> watchers;
      {
        AutoLock lock(state_lock_);
        for (const auto& kv : watches_) {
          if (kv.second.handle == triggered_fd) {
            watchers.push_back(kv.second);
          }
        }
      }

      if (watchers.empty())
        continue;

      // Dispatch by event mask: EPOLLIN → OnFileCanReadWithoutBlocking,
      // EPOLLOUT → OnFileCanWriteWithoutBlocking.  A single epoll event
      // may carry both flags (e.g. EPOLLHUP makes the fd both readable
      // and writable), so we check each flag independently against the
      // watcher's registered mode.
      for (const auto& record : watchers) {
        if (record.watcher == nullptr)
          continue;

        const bool can_read =
            (event.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
        const bool can_write =
            (event.events & (EPOLLOUT | EPOLLERR)) != 0;

        if (can_read &&
            (record.mode == MessagePumpForIO::FdWatchController::Mode::READ ||
             record.mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE)) {
          record.watcher->OnFileCanReadWithoutBlocking(record.handle);
        }
        if (can_write &&
            (record.mode == MessagePumpForIO::FdWatchController::Mode::WRITE ||
             record.mode == MessagePumpForIO::FdWatchController::Mode::READ_WRITE)) {
          record.watcher->OnFileCanWriteWithoutBlocking(record.handle);
        }
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

  int epoll_fd_ = -1;
  int event_fd_ = -1;
  std::atomic<std::uint64_t> next_watch_id_{2};
  std::unordered_map<std::uint64_t, WatchRecord> watches_;
};

MessagePumpForIO::FdWatchController::FdWatchController() = default;

MessagePumpForIO::FdWatchController::~FdWatchController() {
  StopWatching();
}

bool MessagePumpForIO::FdWatchController::StartWatching(MessagePumpForIO* pump,
                                                        NativeIOHandle handle,
                                                        Mode mode,
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
    impl_->StopWatching(watch_id_, handle_);
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

MessagePumpForIO* MessagePumpForIO::Current() {
  return reinterpret_cast<MessagePumpForIO*>(GetCurrentPumpSlot().Get());
}

void MessagePumpForIO::Run(Delegate* delegate) {
  if (delegate == nullptr || impl_ == nullptr) {
    return;
  }

  MessagePumpForIO* previous = Current();
  GetCurrentPumpSlot().Set(this);

  const PlatformThread::PlatformThreadId current_thread_id = PlatformThread::CurrentId();
  DCHECK(impl_->IsCurrentRunThread(current_thread_id));

  const int run_depth = impl_->EnterRunLoopAndGetDepth(current_thread_id);

  while (impl_->IsRunLoopActive(run_depth)) {
    {
      TRACE_EVENT0("nei.message_pump", "MessagePumpForIO::DoWork");
      if (delegate->DoWork()) {
        continue;
      }
    }

    Delegate::NextWorkInfo next_work_info;
    {
      TRACE_EVENT0("nei.message_pump", "MessagePumpForIO::DoDelayedWork");
      if (delegate->DoDelayedWork(&next_work_info)) {
        impl_->UpdateDelayedWorkFromDelegate(next_work_info);
        continue;
      }
    }
    impl_->UpdateDelayedWorkFromDelegate(next_work_info);

    {
      TRACE_EVENT0("nei.message_pump", "MessagePumpForIO::DoIdleWork");
      if (delegate->DoIdleWork()) {
        continue;
      }
    }

    if (!impl_->IsRunLoopActive(run_depth)) {
      break;
    }

    impl_->DrainPendingWakeups(delegate);
    // DrainPendingWakeups may have consumed the only eventfd wake-up
    // without processing tasks (should_run_task=false).  Always try
    // DoWork to catch pending tasks, otherwise the pump may block
    // forever in WaitAndDispatch below.
    (void)delegate->DoWork();
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
  GetCurrentPumpSlot().Set(previous);
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

#endif  // !defined(_WIN32)
