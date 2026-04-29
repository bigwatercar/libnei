#include <neixx/io/io_operation.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include <neixx/task/task_runner.h>

namespace nei {

namespace {

constexpr int kCancelledResult = -125;
constexpr int kTimedOutResult = -110;

} // namespace

// ---------------------------------------------------------------------------
// IOOperationState::Impl
// ---------------------------------------------------------------------------

enum class FinalState : int {
  kPending = 0,
  kCompleted,
  kCancelled,
  kTimedOut,
};

class IOOperationState::Impl {
public:
  std::atomic<int> state{static_cast<int>(FinalState::kPending)};
  std::atomic<int> last_result{0};

  std::mutex hook_mutex;
  std::function<void()> cancel_hook;
  std::atomic<bool> cancel_requested{false};
  std::atomic<bool> cancel_hook_fired{false};

  bool TransitionPendingTo(FinalState target, int result) {
    int expected = static_cast<int>(FinalState::kPending);
    if (!state.compare_exchange_strong(expected,
                                       static_cast<int>(target),
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
      return false;
    }
    last_result.store(result, std::memory_order_release);
    return true;
  }

  void FireCancelHookOnce() {
    std::function<void()> hook;
    {
      std::lock_guard<std::mutex> lock(hook_mutex);
      hook = cancel_hook;
    }
    if (!hook) {
      return;
    }
    bool expected = false;
    if (!cancel_hook_fired.compare_exchange_strong(expected,
                                                   true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
      return;
    }
    hook();
  }
};

// ---------------------------------------------------------------------------
// IOOperationToken
// ---------------------------------------------------------------------------

IOOperationToken::IOOperationToken(scoped_refptr<IOOperationState> state)
    : state_(state.get()) {
  if (state_) {
    state_->AddRef();
  }
}

IOOperationToken::~IOOperationToken() {
  if (state_) {
    state_->Release();
  }
}

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

// ---------------------------------------------------------------------------
// IOOperationState
// ---------------------------------------------------------------------------

IOOperationState::IOOperationState() : impl_(std::make_unique<Impl>()) {
}

IOOperationState::~IOOperationState() = default;

void IOOperationState::BindCancelHook(std::function<void()> hook) {
  {
    std::lock_guard<std::mutex> lock(impl_->hook_mutex);
    impl_->cancel_hook = std::move(hook);
  }
  if (impl_->cancel_requested.load(std::memory_order_acquire)) {
    impl_->FireCancelHookOnce();
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
  if (!impl_->TransitionPendingTo(FinalState::kCompleted, result)) {
    return false;
  }
  if (cb) {
    cb(result);
  }
  return true;
}

bool IOOperationState::TryCancel(std::function<void(int)> cb) {
  if (!impl_->TransitionPendingTo(FinalState::kCancelled, kCancelledResult)) {
    return false;
  }
  impl_->FireCancelHookOnce();
  if (cb) {
    cb(kCancelledResult);
  }
  return true;
}

bool IOOperationState::TryTimeout(std::function<void(int)> cb) {
  if (!impl_->TransitionPendingTo(FinalState::kTimedOut, kTimedOutResult)) {
    return false;
  }
  impl_->FireCancelHookOnce();
  if (cb) {
    cb(kTimedOutResult);
  }
  return true;
}

void IOOperationState::RequestCancel() {
  impl_->cancel_requested.store(true, std::memory_order_release);
  impl_->FireCancelHookOnce();
}

bool IOOperationState::IsDone() const {
  return impl_->state.load(std::memory_order_acquire) != static_cast<int>(FinalState::kPending);
}

bool IOOperationState::IsCancelled() const {
  return impl_->state.load(std::memory_order_acquire) == static_cast<int>(FinalState::kCancelled);
}

bool IOOperationState::IsTimedOut() const {
  return impl_->state.load(std::memory_order_acquire) == static_cast<int>(FinalState::kTimedOut);
}

int IOOperationState::LastResult() const {
  return impl_->last_result.load(std::memory_order_acquire);
}

} // namespace nei
