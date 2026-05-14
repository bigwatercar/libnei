#pragma once

#ifndef NEIXX_TASK_THREAD_POOL_INSTANCE_H_
#define NEIXX_TASK_THREAD_POOL_INSTANCE_H_

#include <nei/macros/nei_export.h>
#include <neixx/common/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>
#include <neixx/task/thread_pool.h>

namespace nei {

// Chromium-style global ThreadPool singleton.
//
// Typical usage:
//   // At program start:
//   nei::ThreadPoolInstance::CreateAndStartWithDefaultParams();
//
//   // Anywhere afterwards:
//   nei::PostTask(FROM_HERE, []() { DoWork(); });
//   auto runner = nei::CreateSequencedTaskRunner({.may_block = true});
//
//   // At program shutdown:
//   nei::ThreadPoolInstance::Shutdown();
class NEI_API ThreadPoolInstance final {
 public:
  // Returns the global singleton, or nullptr if not yet initialized / already
  // shut down.
  static ThreadPoolInstance* Get();

  // Creates the global singleton with default parameters (thread count derived
  // from hardware_concurrency). Must only be called once.
  static void CreateAndStartWithDefaultParams();

  // Drains pending tasks, shuts down, and destroys the global singleton.
  static void Shutdown();

  // Creates a sequenced TaskRunner on the global pool.
  scoped_refptr<TaskRunner> CreateSequencedTaskRunner(
      const TaskTraits& traits = TaskTraits());

  ThreadPoolInstance(const ThreadPoolInstance&) = delete;
  ThreadPoolInstance& operator=(const ThreadPoolInstance&) = delete;

 private:
  ThreadPoolInstance();
  ~ThreadPoolInstance();

  ThreadPool pool_;
};

// ---------------------------------------------------------------------------
// Global convenience wrappers — require ThreadPoolInstance to be alive.
// ---------------------------------------------------------------------------

// Posts a fire-and-forget task to the global pool.
NEI_API void PostTask(const Location& from_here, OnceClosure task);
NEI_API void PostTask(const Location& from_here, OnceClosure task,
                      const TaskTraits& traits);

// Returns a sequenced TaskRunner from the global pool.
NEI_API scoped_refptr<TaskRunner> CreateSequencedTaskRunner(
    const TaskTraits& traits = TaskTraits());

}  // namespace nei

#endif  // NEIXX_TASK_THREAD_POOL_INSTANCE_H_
