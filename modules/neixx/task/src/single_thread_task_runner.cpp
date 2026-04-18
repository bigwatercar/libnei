#include "single_thread_task_runner.h"

#include <utility>

namespace nei {

class SingleThreadTaskRunner::Impl {
public:
  explicit Impl(EnqueueDelegate enqueue_delegate)
      : enqueue_delegate_(enqueue_delegate) {
  }

  void PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
    if (enqueue_delegate_.invoke == nullptr) {
      return;
    }
    enqueue_delegate_.invoke(
        enqueue_delegate_.context, from_here, traits, std::move(task), std::chrono::milliseconds(0));
  }

  void PostDelayedTaskWithTraits(const Location &from_here,
                                 const TaskTraits &traits,
                                 OnceClosure task,
                                 std::chrono::milliseconds delay) {
    if (enqueue_delegate_.invoke == nullptr) {
      return;
    }
    enqueue_delegate_.invoke(enqueue_delegate_.context, from_here, traits, std::move(task), delay);
  }

private:
  EnqueueDelegate enqueue_delegate_;
};

SingleThreadTaskRunner::SingleThreadTaskRunner(EnqueueDelegate enqueue_delegate)
    : impl_(std::make_unique<Impl>(enqueue_delegate)) {
}

SingleThreadTaskRunner::~SingleThreadTaskRunner() = default;

SingleThreadTaskRunner::SingleThreadTaskRunner(SingleThreadTaskRunner &&) noexcept = default;

SingleThreadTaskRunner &SingleThreadTaskRunner::operator=(SingleThreadTaskRunner &&) noexcept = default;

void SingleThreadTaskRunner::PostTaskWithTraits(const Location &from_here, const TaskTraits &traits, OnceClosure task) {
  impl_->PostTaskWithTraits(from_here, traits, std::move(task));
}

void SingleThreadTaskRunner::PostDelayedTaskWithTraits(const Location &from_here,
                                                       const TaskTraits &traits,
                                                       OnceClosure task,
                                                       std::chrono::milliseconds delay) {
  impl_->PostDelayedTaskWithTraits(from_here, traits, std::move(task), delay);
}

} // namespace nei
