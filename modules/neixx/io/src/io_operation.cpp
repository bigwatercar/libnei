#include <neixx/io/io_operation.h>

#include <neixx/task/task_runner.h>

namespace nei {

namespace {

constexpr int kCancelledResult = -125;
constexpr int kTimedOutResult = -110;

} // namespace

IOOperationToken::IOOperationToken(scoped_refptr<IOOperationState> state)
    : state_(std::move(state)) {
}

IOOperationToken::~IOOperationToken() = default;

void IOOperationToken::Cancel() {
  if (state_) {
    state_->RequestCancel();
  }
}

bool IOOperationToken::IsDone() const {
  return state_ && state_->IsDone();
}

bool IOOperationToken::IsCancelled() const {
  return state_ && state_->IsCancelled();
}

bool IOOperationToken::IsTimedOut() const {
  return state_ && state_->IsTimedOut();
}

int IOOperationToken::LastResult() const {
  return state_ ? state_->LastResult() : 0;
}

IOOperationState::IOOperationState() = default;

IOOperationState::~IOOperationState() = default;

void IOOperationState::BindCancelHook(std::function<void()> hook) {
  {
    std::lock_guard<std::mutex> lock(hook_mutex_);
    cancel_hook_ = std::move(hook);
  }

  // If cancellation was requested before hook binding, fire now.
  if (cancel_requested_.load(std::memory_order_acquire)) {
    FireCancelHookOnce();
  }
}

void IOOperationState::StartTimeoutWatch(std::chrono::milliseconds timeout,
                                          TaskRunner* task_runner,
                                          std::function<void(int)> cb) {
  if (timeout.count() <= 0 || !task_runner) {
    return;
  }

  scoped_refptr<IOOperationState> self(this);
  task_runner->PostDelayedTask(
      FROM_HERE,
      [self = std::move(self), cb = std::move(cb)]() mutable {
        (void)self->TryTimeout(std::move(cb));
      },
      timeout);
}

bool IOOperationState::TryComplete(int result, std::function<void(int)> cb) {
  if (!TransitionPendingTo(FinalState::kCompleted, result)) {
    return false;
  }
  if (cb) {
    cb(result);
  }
  return true;
}

bool IOOperationState::TryCancel(std::function<void(int)> cb) {
  if (!TransitionPendingTo(FinalState::kCancelled, kCancelledResult)) {
    return false;
  }
  FireCancelHookOnce();
  if (cb) {
    cb(kCancelledResult);
  }
  return true;
}

bool IOOperationState::TryTimeout(std::function<void(int)> cb) {
  if (!TransitionPendingTo(FinalState::kTimedOut, kTimedOutResult)) {
    return false;
  }
  FireCancelHookOnce();
  if (cb) {
    cb(kTimedOutResult);
  }
  return true;
}

void IOOperationState::RequestCancel() {
  cancel_requested_.store(true, std::memory_order_release);
  FireCancelHookOnce();
}

bool IOOperationState::IsDone() const {
  return state_.load(std::memory_order_acquire) != static_cast<int>(FinalState::kPending);
}

bool IOOperationState::IsCancelled() const {
  return state_.load(std::memory_order_acquire) == static_cast<int>(FinalState::kCancelled);
}

bool IOOperationState::IsTimedOut() const {
  return state_.load(std::memory_order_acquire) == static_cast<int>(FinalState::kTimedOut);
}

int IOOperationState::LastResult() const {
  return last_result_.load(std::memory_order_acquire);
}

bool IOOperationState::TransitionPendingTo(FinalState target, int result) {
  int expected = static_cast<int>(FinalState::kPending);
  if (!state_.compare_exchange_strong(expected,
                                      static_cast<int>(target),
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
    return false;
  }

  last_result_.store(result, std::memory_order_release);
  return true;
}

void IOOperationState::FireCancelHookOnce() {
  std::function<void()> hook;
  {
    std::lock_guard<std::mutex> lock(hook_mutex_);
    hook = cancel_hook_;
  }
  if (!hook) {
    return;
  }

  bool expected = false;
  if (!cancel_hook_fired_.compare_exchange_strong(expected,
                                                  true,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
    return;
  }

  hook();
}

} // namespace nei
