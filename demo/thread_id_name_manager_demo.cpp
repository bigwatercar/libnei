#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread_id_name_manager.h>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

int main() {
  using nei::PlatformThread;
  using nei::PlatformThreadId;
  using nei::ThreadIdNameManager;

  std::atomic<bool> stop{false};
  std::promise<PlatformThreadId> id_promise_1;
  std::promise<PlatformThreadId> id_promise_2;
  std::future<PlatformThreadId> id_future_1 = id_promise_1.get_future();
  std::future<PlatformThreadId> id_future_2 = id_promise_2.get_future();

  std::thread worker_1([&]() {
    PlatformThread::SetName("demo_worker_1");
    id_promise_1.set_value(PlatformThread::CurrentId());
    while (!stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  std::thread worker_2([&]() {
    PlatformThread::SetName("demo_worker_2");
    id_promise_2.set_value(PlatformThread::CurrentId());
    while (!stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  const PlatformThreadId id1 = id_future_1.get();
  const PlatformThreadId id2 = id_future_2.get();

  ThreadIdNameManager *manager = ThreadIdNameManager::GetInstance();
  const std::string name1 = manager->GetName(id1);
  const std::string name2 = manager->GetName(id2);

  std::cout << "id=" << id1 << ", name=" << name1 << '\n';
  std::cout << "id=" << id2 << ", name=" << name2 << '\n';

  stop.store(true, std::memory_order_release);
  worker_1.join();
  worker_2.join();

  const bool ok = (name1 == "demo_worker_1" && name2 == "demo_worker_2");
  std::cout << "mapping check: " << (ok ? "PASS" : "FAIL") << '\n';
  return ok ? 0 : 1;
}
