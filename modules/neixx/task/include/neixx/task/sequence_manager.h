#pragma once

#ifndef NEIXX_TASK_SEQUENCE_MANAGER_H_
#define NEIXX_TASK_SEQUENCE_MANAGER_H_

#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/task/message_loop/message_pump.h>
#include <neixx/task/task_runner.h>
#include <neixx/task/task_traits.h>

namespace nei {

class NEI_API SequenceManager final : public MessagePump::Delegate {
 public:
  explicit SequenceManager(std::unique_ptr<MessagePump> pump = nullptr);
  ~SequenceManager() override;

  static SequenceManager* Current();

  SequenceManager(const SequenceManager&) = delete;
  SequenceManager& operator=(const SequenceManager&) = delete;
  SequenceManager(SequenceManager&&) = delete;
  SequenceManager& operator=(SequenceManager&&) = delete;

  scoped_refptr<TaskRunner> CreateTaskRunner(const TaskTraits& traits = TaskTraits());
  scoped_refptr<TaskRunner> GetDefaultTaskRunner();

  void Run();
  void Quit();
  void Shutdown();

  bool DoWork() override;
  bool DoDelayedWork(NextWorkInfo* next_work_info) override;
  bool DoIdleWork() override;

  // Testing-only knobs to force coverage of both DoWork paths.
  static void SetSingleQueueFastPathEnabledForTesting(bool enabled);
  static bool IsSingleQueueFastPathEnabledForTesting();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nei

#endif  // NEIXX_TASK_SEQUENCE_MANAGER_H_
