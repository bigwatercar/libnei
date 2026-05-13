#include <neixx/threading/thread.h>

#include <utility>

#include <neixx/common/location.h>
#include <neixx/task/run_loop.h>
#include <neixx/task/sequence_manager.h>

namespace nei {

Thread::Thread(const std::string& name) : name_(name) {}

Thread::~Thread() {
  Stop();
}

bool Thread::Start() {
  {
    AutoLock lock(lock_);
    if (started_) {
      return false;
    }
    start_event_ =
        std::make_unique<WaitableEvent>(WaitableEvent::ResetPolicy::kManual, false);
    started_ = true;
    start_succeeded_ = false;
    running_ = false;
  }

  if (!PlatformThread::Create(0, this, &handle_)) {
    AutoLock lock(lock_);
    start_event_.reset();
    started_ = false;
    return false;
  }

  start_event_->Wait();

  bool start_succeeded = false;
  {
    AutoLock lock(lock_);
    start_succeeded = start_succeeded_;
    start_event_.reset();
  }

  if (!start_succeeded) {
    (void)PlatformThread::Join(&handle_);
    AutoLock lock(lock_);
    started_ = false;
  }

  return start_succeeded;
}

void Thread::Stop() {
  scoped_refptr<TaskRunner> runner;
  {
    AutoLock lock(lock_);
    if (!started_) {
      return;
    }
    // Mark as no longer started under the lock so concurrent Stop() calls
    // return early instead of reaching Join() with the same handle.
    started_ = false;
    runner = task_runner_;
  }

  if (runner) {
    runner->PostTask(FROM_HERE, []() {
      SequenceManager* current = SequenceManager::Current();
      if (current != nullptr) {
        current->Quit();
      }
    });
  }

  (void)PlatformThread::Join(&handle_);

  AutoLock lock(lock_);
  task_runner_ = nullptr;
  running_ = false;
  start_succeeded_ = false;
  start_event_.reset();
}

scoped_refptr<TaskRunner> Thread::GetTaskRunner() const {
  AutoLock lock(lock_);
  return task_runner_;
}

bool Thread::IsRunning() const {
  AutoLock lock(lock_);
  return running_;
}

void Thread::ThreadMain() {
  if (!name_.empty()) {
    PlatformThread::SetCurrentThreadName(name_);
  }

  SequenceManager sequence_manager;
  scoped_refptr<TaskRunner> default_task_runner = sequence_manager.GetDefaultTaskRunner();

  WaitableEvent* start_event = nullptr;
  {
    AutoLock lock(lock_);
    task_runner_ = default_task_runner;
    start_succeeded_ = default_task_runner.get() != nullptr;
    running_ = start_succeeded_;
    start_event = start_event_.get();
  }

  if (start_event != nullptr) {
    start_event->Signal();
  }

  if (default_task_runner.get() == nullptr) {
    AutoLock lock(lock_);
    task_runner_ = nullptr;
    running_ = false;
    return;
  }

  RunLoop run_loop;
  run_loop.Run();

  AutoLock lock(lock_);
  task_runner_ = nullptr;
  running_ = false;
}

}  // namespace nei
