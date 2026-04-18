#include <neixx/task/location.h>
#include <neixx/functional/callback.h>
#include <neixx/task/sequenced_task_runner.h>
#include <neixx/task/task_tracer.h>
#include <neixx/task/thread_pool.h>

#include <chrono>
#include <functional>
#include <future>
#include <iostream>

int main() {
    nei::ThreadPool thread_pool(2);
    auto runner = thread_pool.CreateSequencedTaskRunner();

    std::promise<void> done;
    auto done_future = done.get_future();

    const auto start = std::chrono::steady_clock::now();
    runner->PostDelayedTask(
        FROM_HERE,
        nei::BindOnce(
            [](std::promise<void>& inner_done, std::chrono::steady_clock::time_point inner_start) {
                const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - inner_start);

                const nei::Location* location = nei::TaskTracer::GetCurrentTaskLocation();
                if (location && !location->is_null()) {
                    std::cout << "task posted from file: " << location->file_name() << '\n';
                    std::cout << "task posted from function: " << location->function_name() << '\n';
                    std::cout << "task posted from line: " << location->line() << '\n';
                } else {
                    std::cout << "task location unavailable\n";
                }
                std::cout << "elapsed(ms): " << elapsed_ms.count() << '\n';
                inner_done.set_value();
            },
            std::ref(done),
            start),
        std::chrono::milliseconds(2000));

    done_future.wait();
    return 0;
}

