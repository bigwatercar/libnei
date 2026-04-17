#include <neixx/task/callback.h>
#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/thread_pool.h>
#include <neixx/task/weak_ptr.h>

#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

namespace {

class WorkerObject {
public:
    explicit WorkerObject(std::shared_ptr<nei::SequencedTaskRunner> runner)
        : runner_(std::move(runner)), weak_factory_(this) {}

    ~WorkerObject() {
        weak_factory_.InvalidateWeakPtrs();
    }

    void StartLongTask(std::promise<bool>& done) {
        nei::WeakPtr<WorkerObject> weak_this = weak_factory_.GetWeakPtr();
        runner_->PostTask(
            FROM_HERE,
            nei::BindOnce(
                [](nei::WeakPtr<WorkerObject> weak_inner, std::promise<bool>& inner_done) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if (!weak_inner) {
                        std::cout << "weak ptr invalidated, task skipped\n";
                        inner_done.set_value(true);
                        return;
                    }
                    weak_inner->DoBusinessWork();
                    inner_done.set_value(false);
                },
                weak_this,
                std::ref(done)));
    }

private:
    void DoBusinessWork() {
        std::cout << "unexpected business work execution\n";
    }

    std::shared_ptr<nei::SequencedTaskRunner> runner_;
    nei::WeakPtrFactory<WorkerObject> weak_factory_;
};

} // namespace

int main() {
    nei::ThreadPool thread_pool(2);
    auto runner = thread_pool.CreateSequencedTaskRunner();

    std::promise<void> gate_started;
    std::promise<void> release_gate;
    auto release_future = release_gate.get_future().share();

    runner->PostTask(
        FROM_HERE,
        nei::BindOnce(
            [](std::promise<void>& inner_gate_started, std::shared_future<void> inner_release_future) {
                inner_gate_started.set_value();
                inner_release_future.wait();
            },
            std::ref(gate_started),
            release_future));

    gate_started.get_future().wait();

    std::promise<bool> done;
    auto done_future = done.get_future();
    {
        auto worker = std::make_unique<WorkerObject>(runner);
        worker->StartLongTask(done);
    }

    release_gate.set_value();

    const bool skipped_safely = done_future.get();
    std::cout << "weak ptr safety: " << (skipped_safely ? "PASS" : "FAIL") << '\n';
    return skipped_safely ? 0 : 1;
}
