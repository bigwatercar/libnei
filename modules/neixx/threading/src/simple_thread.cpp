#include <neixx/threading/simple_thread.h>

#include <nei/debug/check.h>
#include <neixx/synchronization/waitable_event.h>

namespace nei {

namespace {

/// A self-resetting barrier that lives on the starter's stack.
/// ThreadMain signals it; StartWithOptions blocks on it.
struct StartBarrier {
  WaitableEvent event{WaitableEvent::ResetPolicy::kManual, false};
};

}  // namespace

SimpleThread::SimpleThread(const std::string& name) : name_(name) {}

SimpleThread::~SimpleThread() {
  // Catch missing Join() in debug builds.  Let PlatformThread::Handle's
  // own DCHECK provide the diagnostic if the handle is still live.
  DCHECK_MSG(joined_ || !started_,
             "SimpleThread was started but never joined. "
             "Call Join() before destruction.");
}

// ---------------------------------------------------------------------------
//  Start / StartWithOptions
// ---------------------------------------------------------------------------

void SimpleThread::Start() {
  StartWithOptions(Options{});
}

void SimpleThread::StartWithOptions(const Options& options) {
  StartBarrier barrier;
  {
    AutoLock lock(lock_);
    DCHECK_MSG(!started_, "SimpleThread::Start() called more than once.");
    if (started_) {
      return;
    }
    options_ = options;
    start_event_ = &barrier.event;
    started_ = true;
    joined_ = false;
    thread_id_ = 0;
  }

  if (!PlatformThread::CreateWithType(options_.stack_size, this, &handle_,
                                      options_.thread_type)) {
    // OS thread creation failed.  Roll back state.
    AutoLock lock(lock_);
    start_event_ = nullptr;
    started_ = false;
    thread_id_ = 0;
    return;
  }

  // Wait for the new thread to finish basic init (thread name, OS prio,
  // thread-id capture).  The matching Signal() is in ThreadMain().
  barrier.event.Wait();

  {
    AutoLock lock(lock_);
    start_event_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
//  Join
// ---------------------------------------------------------------------------

void SimpleThread::Join() {
  {
    AutoLock lock(lock_);
    DCHECK_MSG(started_, "SimpleThread::Join() called before Start().");
    DCHECK_MSG(!joined_, "SimpleThread::Join() called more than once.");
  }

  // Self-join is guaranteed to deadlock &mdash; let PlatformThread catch it.
  DCHECK_NE_MSG(thread_id_, PlatformThread::CurrentId(),
                "Self-join would cause deadlock");

  (void)PlatformThread::Join(&handle_);

  AutoLock lock(lock_);
  joined_ = true;
}

// ---------------------------------------------------------------------------
//  Query
// ---------------------------------------------------------------------------

bool SimpleThread::HasBeenStarted() const {
  AutoLock lock(lock_);
  return started_ && !joined_;
}

bool SimpleThread::HasBeenJoined() const {
  AutoLock lock(lock_);
  return joined_;
}

PlatformThread::PlatformThreadId SimpleThread::GetThreadId() const {
  AutoLock lock(lock_);
  return thread_id_;
}

// ---------------------------------------------------------------------------
//  PlatformThread::Delegate
// ---------------------------------------------------------------------------

void SimpleThread::ThreadMain() {
  // --- 1.  Basic init  ------------------------------------------------
  if (!name_.empty()) {
    PlatformThread::SetCurrentThreadName(name_);
  }

  // Apply the requested OS-level scheduling weight.
  // options_ is visible here with happens-before through OS thread creation.
  PlatformThread::SetCurrentThreadType(options_.thread_type);

  WaitableEvent* start_event = nullptr;
  {
    AutoLock lock(lock_);
    thread_id_ = PlatformThread::CurrentId();
    start_event = start_event_;
  }

  // --- 2.  Signal the starter that we are ready  ---------------------
  // Unblock StartWithOptions() which is waiting on this event.
  if (start_event) {
    start_event->Signal();
  }

  // --- 3.  User code  ------------------------------------------------
  Run();
}

}  // namespace nei
