#include <neixx/functional/cancelable_callback.h>
#include <neixx/functional/callback.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace nei {

class CancelableOnceClosure::Impl {
public:
  explicit Impl(OnceCallback closure)
      : state_(std::make_shared<State>(std::move(closure))) {
  }

  OnceCallback callback() {
    std::shared_ptr<State> state = state_;
    return BindOnce([state = std::move(state)]() mutable {
      if (state->is_cancelled.load(std::memory_order_acquire)) {
        return;
      }

      OnceCallback task;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->is_cancelled.load(std::memory_order_relaxed)) {
          return;
        }
        task = std::move(state->task);
      }

      if (!task) {
        return;
      }

      if (state->is_cancelled.load(std::memory_order_acquire)) {
        return;
      }

      std::move(task).Run();
    });
  }

  void Cancel() {
    state_->is_cancelled.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->task = OnceCallback();
  }

private:
  struct State {
    explicit State(OnceCallback closure_in)
        : task(std::move(closure_in)) {
    }

    std::atomic<bool> is_cancelled{false};
    std::mutex mutex;
    OnceCallback task;
  };

  std::shared_ptr<State> state_;
};

CancelableOnceClosure::CancelableOnceClosure(OnceCallback closure)
    : impl_(std::make_unique<Impl>(std::move(closure))) {
}

CancelableOnceClosure::~CancelableOnceClosure() {
  Cancel();
}

CancelableOnceClosure::CancelableOnceClosure(CancelableOnceClosure &&other) noexcept = default;

CancelableOnceClosure &CancelableOnceClosure::operator=(CancelableOnceClosure &&other) noexcept {
  if (this != &other) {
    Cancel();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

OnceCallback CancelableOnceClosure::callback() {
  if (!impl_) {
    return OnceCallback();
  }
  return impl_->callback();
}

void CancelableOnceClosure::Cancel() {
  if (!impl_) {
    return;
  }
  impl_->Cancel();
}

} // namespace nei
