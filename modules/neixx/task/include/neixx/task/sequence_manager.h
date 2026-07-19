#pragma once

#ifndef NEIXX_TASK_SEQUENCE_MANAGER_H_
#define NEIXX_TASK_SEQUENCE_MANAGER_H_

#include <memory>

#include <nei/macros/nei_export.h>
#include <nei/macros/suppress_compiler_warnings.h>
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
  NEI_SUPPRESS_MSC_WARNING_BEGIN(4251)
  std::unique_ptr<Impl> impl_;
  NEI_SUPPRESS_MSC_WARNING_END
};

}  // namespace nei

#endif  // NEIXX_TASK_SEQUENCE_MANAGER_H_
