#include <neixx/task/thread_task_runner_handle.h>

#include <neixx/task/sequence_manager.h>
#include <neixx/task/task_runner.h>

namespace nei {

scoped_refptr<TaskRunner> ThreadTaskRunnerHandle::Get() {
  SequenceManager* sequence_manager = SequenceManager::Current();
  if (sequence_manager == nullptr) {
    return nullptr;
  }
  return sequence_manager->CreateTaskRunner();
}

}  // namespace nei
