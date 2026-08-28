#ifndef NEI_TASK_INTERNAL_THREAD_GROUP_H_
#define NEI_TASK_INTERNAL_THREAD_GROUP_H_

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "neixx/synchronization/lock.h"

namespace nei {
namespace internal {

// ThreadGroup manages a collection of worker threads with a shared
// lifecycle.  It owns thread join handles, tracks counts, and enforces
// a maximum worker cap.
//
// This class is the Chromium-aligned abstraction for the worker-
// management half of a ThreadPool.  It is type-erased: the concrete
// worker type (defined in thread_pool.cpp) is never visible here.
// Factories return an opaque ThreadHandle that ThreadGroup stores
// and later joins.
//
// All public methods are thread-safe unless noted.

struct ThreadHandle {
  // Joins (waits for) the thread.  Must be called at most once.
  std::function<void()> join;
};

class ThreadGroup final {
public:
  // Factory type used to create and start a worker thread.
  // Arguments: (index, is_compensation).
  // Returns an empty ThreadHandle (join == nullptr) on failure.
  using WorkerFactory = std::function<ThreadHandle(std::size_t, bool)>;

  // Constructs a group with an upper bound on concurrent workers.
  // |name| is used for diagnostics.
  // |max_worker_count| is the hard cap; StartWorkers and
  // SpawnCompensationWorker both respect it.
  ThreadGroup(std::string name, std::size_t max_worker_count);
  ~ThreadGroup();

  ThreadGroup(const ThreadGroup &) = delete;
  ThreadGroup &operator=(const ThreadGroup &) = delete;

  // ---- Lifecycle ----

  // Creates |count| workers using |factory|, registers their join
  // handles under group_lock_, and starts them (Start is invoked by
  // the factory before registration).  Workers are registered before
  // threads are started so that any callback that inspects the group
  // sees a consistent view.
  //
  // Thread-safe; may be called without any external lock held.
  void StartWorkers(std::size_t count, const WorkerFactory &factory);

  // Joins all live workers.  After this returns, the group is empty.
  void JoinAll();

  // ---- Queries ----

  std::size_t worker_count() const;

  // ---- Runtime spawning ----

  // Spawns a compensation worker (is_compensation=true).
  // Respects |max_worker_count_|.  Capacity check and registration
  // are both performed under group_lock_ to prevent TOCTOU races.
  //
  // Returns true if a new worker was spawned, false otherwise
  // (cap reached or factory returned empty handle).
  //
  // Thread-safe; may be called without any external lock held.
  bool SpawnCompensationWorker(const WorkerFactory &factory);

private:
  const std::string name_;
  const std::size_t max_worker_count_;

  mutable Lock group_lock_;
  std::vector<ThreadHandle> handles_;

  // Total number of workers created (including those already
  // joined).  This is used to assign monotonically increasing
  // indices.
  std::size_t next_index_ = 0;
};

} // namespace internal
} // namespace nei

#endif // NEI_TASK_INTERNAL_THREAD_GROUP_H_
