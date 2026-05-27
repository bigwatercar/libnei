#include <atomic>

#include <neixx/functional/bind.h>
#include <neixx/functional/callback.h>
#include <neixx/task/thread_pool.h>

std::atomic<int> counter(0);

void CountTask(nei::scoped_refptr<nei::TaskRunner> runner) {
  auto old = counter.fetch_add(1, std::memory_order_relaxed);
  if (old < 10) {
    printf("Counter: %d\n", old + 1);
    runner->PostDelayedTask(FROM_HERE, nei::BindOnce(CountTask, runner), nei::TimeDelta::FromSeconds(1));
  }
}

int main() {
  printf("NEI Library Demo - Demonstrating libnei functionality\n");

  nei::ThreadPool::InitParams params;
  params.max_num_workers = 4;
  params.enable_single_queue_fast_path = true;
  params.suggested_reclaim_time = nei::TimeDelta();
  params.worker_thread_type = nei::ThreadType::DEFAULT;

  auto pool = nei::ThreadPool(params);
  auto runner = pool.CreateSequencedTaskRunner();
  runner->PostTask(FROM_HERE, nei::BindOnce(CountTask, runner));

  while (counter.load(std::memory_order_relaxed) < 10) {
    nei::PlatformThread::Sleep(nei::TimeDelta::FromMilliseconds(100));
  }

  return 0;
}