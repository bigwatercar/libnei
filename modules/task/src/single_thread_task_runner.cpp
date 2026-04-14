#include "single_thread_task_runner.h"

#include <utility>

namespace nei {

class SingleThreadTaskRunner::Impl {
public:
    explicit Impl(std::function<void(const Location&, OnceClosure, std::chrono::milliseconds)> enqueue)
        : enqueue_(std::move(enqueue)) {}

    void PostTask(const Location& from_here, OnceClosure task) {
        enqueue_(from_here, std::move(task), std::chrono::milliseconds(0));
    }

    void PostDelayedTask(
        const Location& from_here,
        OnceClosure task,
        std::chrono::milliseconds delay) {
        enqueue_(from_here, std::move(task), delay);
    }

private:
    std::function<void(const Location&, OnceClosure, std::chrono::milliseconds)> enqueue_;
};

SingleThreadTaskRunner::SingleThreadTaskRunner(
    std::function<void(const Location&, OnceClosure, std::chrono::milliseconds)> enqueue)
    : impl_(std::make_unique<Impl>(std::move(enqueue))) {}

SingleThreadTaskRunner::~SingleThreadTaskRunner() = default;

SingleThreadTaskRunner::SingleThreadTaskRunner(SingleThreadTaskRunner&&) noexcept = default;

SingleThreadTaskRunner& SingleThreadTaskRunner::operator=(
    SingleThreadTaskRunner&&) noexcept = default;

void SingleThreadTaskRunner::PostTask(const Location& from_here, OnceClosure task) {
    impl_->PostTask(from_here, std::move(task));
}

void SingleThreadTaskRunner::PostDelayedTask(
    const Location& from_here,
    OnceClosure task,
    std::chrono::milliseconds delay) {
    impl_->PostDelayedTask(from_here, std::move(task), delay);
}

} // namespace nei
