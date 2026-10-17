#ifndef NEI_TASK_INTERNAL_THREAD_GROUP_H_
#define NEI_TASK_INTERNAL_THREAD_GROUP_H_

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "neixx/common/time.h"
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
  // Joins (waits for) the thread.  Safe to call more than once: an already
  // joined or already detached thread is a no-op.
  std::function<void()> join;

  // Waits up to |timeout| for the thread to exit and joins it if it did.  A
  // zero or negative timeout only probes and never blocks.  Returns true when
  // the thread has exited (and is now joined), false while it still runs.
  std::function<bool(TimeDelta)> try_join;
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

  // Joins workers until |timeout| elapses.  Returns true if every worker was
  // joined.  On false, the workers that were still running are kept in the
  // group (not detached and not forgotten), so a later JoinAll() — or another
  // JoinAllWithTimeout() — can finish the job.  |timeout| is measured from the
  // call, and a zero or negative value only probes without waiting.
  bool JoinAllWithTimeout(TimeDelta timeout);

  // ---- Queries ----

  // Number of workers that are still running, i.e. handles whose threads have
  // not exited yet.  Exited workers are reaped (joined and forgotten) first, so
  // this does not grow with the number of compensation workers the pool spawned
  // over its lifetime.
  std::size_t worker_count();

  // Joins and forgets every handle whose worker has already exited.  Called
  // before capacity checks so a self-terminated compensation worker does not
  // keep occupying a slot forever.
  void ReapExitedWorkers();

  // Total number of workers created over this group's lifetime, including the
  // ones that already exited and were reaped.  Testing hook: lets a test tell
  // "no worker was spawned" apart from "a worker was spawned and immediately
  // exited", which is invisible to worker_count().
  std::size_t created_worker_count() const;

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
