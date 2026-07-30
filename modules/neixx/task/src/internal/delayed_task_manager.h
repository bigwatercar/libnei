#pragma once

#ifndef NEIXX_TASK_INTERNAL_DELAYED_TASK_MANAGER_H_
#define NEIXX_TASK_INTERNAL_DELAYED_TASK_MANAGER_H_

#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

#include <neixx/common/time.h>
#include <neixx/synchronization/lock.h>
#include <neixx/synchronization/waitable_event.h>
#include "task_queue.h"
#include <neixx/threading/platform_thread.h>

namespace nei {
namespace internal {

class PooledTaskSource;

// Observes delayed tasks across ThreadPool queues and wakes up at the earliest
// deadline to re-enqueue due queues back to PooledTaskSource.
class DelayedTaskManager final : public PlatformThread::Delegate {
public:
  explicit DelayedTaskManager(PooledTaskSource *task_source);
  ~DelayedTaskManager() override;

  DelayedTaskManager(const DelayedTaskManager &) = delete;
  DelayedTaskManager &operator=(const DelayedTaskManager &) = delete;
  DelayedTaskManager(DelayedTaskManager &&) = delete;
  DelayedTaskManager &operator=(DelayedTaskManager &&) = delete;

  void AddQueue(TaskQueue *queue);
  void RemoveQueue(TaskQueue *queue);
  void OnQueueUpdated(TaskQueue *queue);

  void Shutdown();

private:
  struct QueueState {
    TimeTicks next_run_time;
  };

  struct HeapEntry {
    TaskQueue *queue = nullptr;
    TimeTicks run_time;
    std::uint64_t order = 0;
  };

  struct HeapEntryLess {
    bool operator()(const HeapEntry &lhs, const HeapEntry &rhs) const {
      if (lhs.run_time != rhs.run_time) {
        return lhs.run_time > rhs.run_time;
      }
      return lhs.order > rhs.order;
    }
  };

  void ThreadMain() override;
  void RefreshQueueStateLocked(TaskQueue *queue);
  bool PopNextValidEntryLocked(HeapEntry *out_entry);

  PooledTaskSource *task_source_ = nullptr;

  Lock lock_;
  WaitableEvent wake_event_{WaitableEvent::ResetPolicy::kAutomatic, false};
  PlatformThread::Handle thread_handle_;
  bool thread_started_ = false;
  bool is_shutdown_ = false;
  std::uint64_t next_order_ = 0;

  std::unordered_map<TaskQueue *, QueueState> queues_;
  std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapEntryLess> heap_;
};

} // namespace internal
} // namespace nei

#endif // NEIXX_TASK_INTERNAL_DELAYED_TASK_MANAGER_H_
