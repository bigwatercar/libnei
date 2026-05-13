#pragma once

#ifndef NEIXX_TASK_THREAD_POOL_H_
#define NEIXX_TASK_THREAD_POOL_H_

#include <cstddef>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>

namespace nei {

class NEI_API ThreadPool final {
 public:
  class Impl;

  explicit ThreadPool(std::size_t worker_count = 0);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  scoped_refptr<TaskRunner> CreateSequencedTaskRunner(
      const TaskTraits& traits = TaskTraits());

  void Shutdown();

  std::size_t worker_count() const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // NEIXX_TASK_THREAD_POOL_H_
