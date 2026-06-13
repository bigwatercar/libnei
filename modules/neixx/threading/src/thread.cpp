#include <neixx/threading/thread.h>

#include <memory>
#include <utility>

#include <nei/debug/check.h>
#include <neixx/common/location.h>
#include <neixx/task/message_loop/message_pump_default.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/run_loop.h>
#include <neixx/task/sequence_manager.h>

namespace nei {

namespace {

/// Factory: creates the correct MessagePump for the requested type.
std::unique_ptr<MessagePump> CreateMessagePumpForType(MessagePumpType type) {
  switch (type) {
    case MessagePumpType::IO:
      return std::make_unique<MessagePumpForIO>();
    case MessagePumpType::DEFAULT:
      return std::make_unique<MessagePumpDefault>();
    case MessagePumpType::UI:
      return std::make_unique<MessagePumpDefault>();
  }
  return std::make_unique<MessagePumpDefault>();
}

}  // namespace

Thread::Thread(const std::string& name) : name_(name) {}

Thread::~Thread() {
  Stop();
}

bool Thread::Start() {
  return StartWithOptions(Options{});
}

bool Thread::StartWithOptions(const Options& options) {
  {
    AutoLock lock(lock_);
    if (started_) {
      return false;
    }
    options_ = options;
    start_event_ =
        std::make_unique<WaitableEvent>(WaitableEvent::ResetPolicy::kManual, false);
    started_ = true;
    start_succeeded_ = false;
    running_ = false;
    thread_id_ = 0;
  }

  if (!PlatformThread::CreateWithType(options.stack_size, this, &handle_,
                                      options.thread_type)) {
    AutoLock lock(lock_);
    start_event_.reset();
    started_ = false;
    thread_id_ = 0;
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
    started_ = false;
    runner = task_runner_;
  }

  // Self-join is guaranteed to deadlock: the calling thread would wait on
  // itself. Catch this programmer error early in debug builds.
  DCHECK_NE(GetThreadId(), PlatformThread::CurrentId());

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

PlatformThread::PlatformThreadId Thread::GetThreadId() const {
  AutoLock lock(lock_);
  return thread_id_;
}

void Thread::ThreadMain() {
  if (!name_.empty()) {
    PlatformThread::SetCurrentThreadName(name_);
  }

  // Apply the requested OS-level scheduling weight before entering the loop.
  // Reads options_ which was written (with happens-before via OS thread
  // creation) before this thread started.
  PlatformThread::SetCurrentThreadType(options_.thread_type);

  // Build the pump for this thread's event loop.
  std::unique_ptr<MessagePump> pump =
      CreateMessagePumpForType(options_.message_pump_type);

  SequenceManager sequence_manager(std::move(pump));
  scoped_refptr<TaskRunner> default_task_runner = sequence_manager.GetDefaultTaskRunner();

  WaitableEvent* start_event = nullptr;
  {
    AutoLock lock(lock_);
    thread_id_ = PlatformThread::CurrentId();
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
    thread_id_ = 0;
    return;
  }

  RunLoop run_loop;
  run_loop.Run();

  AutoLock lock(lock_);
  task_runner_ = nullptr;
  running_ = false;
  thread_id_ = 0;
}

}  // namespace nei
