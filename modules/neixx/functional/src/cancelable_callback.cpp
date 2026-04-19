#include <neixx/functional/cancelable_callback.h>
#include <neixx/functional/callback.h>
#include <neixx/memory/weak_ptr.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace nei {

class CancelableOnceClosure::Impl {
public:
  explicit Impl(OnceCallback closure)
      : state_(std::move(closure)) {
  }

  OnceCallback callback() {
    WeakPtr<State> weak_state = state_.GetWeakPtr();
    return BindOnce([weak_state]() mutable {
      if (!weak_state) {
        return;
      }

      if (weak_state->is_cancelled.load(std::memory_order_acquire)) {
        return;
      }

      OnceCallback task;
      {
        std::lock_guard<std::mutex> lock(weak_state->mutex);
        if (weak_state->is_cancelled.load(std::memory_order_relaxed)) {
          return;
        }
        task = std::move(weak_state->task);
      }

      if (!task) {
        return;
      }

      if (weak_state->is_cancelled.load(std::memory_order_acquire)) {
        return;
      }

      std::move(task).Run();
    });
  }

  void Cancel() {
    state_.Cancel();
  }

private:
  struct State {
    explicit State(OnceCallback closure_in)
        : task(std::move(closure_in))
        , weak_factory(this) {
    }

    WeakPtr<State> GetWeakPtr() {
      return weak_factory.GetWeakPtr();
    }

    void Cancel() {
      is_cancelled.store(true, std::memory_order_release);
      std::lock_guard<std::mutex> lock(mutex);
      task = OnceCallback();
      weak_factory.InvalidateWeakPtrs();
    }

    std::atomic<bool> is_cancelled{false};
    std::mutex mutex;
    OnceCallback task;
    WeakPtrFactory<State> weak_factory;
  };

  State state_;
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
