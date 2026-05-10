#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include <neixx/threading/thread_local_storage.h>

namespace nei {
namespace {

std::atomic<int> g_tls_destructor_calls{0};

#if defined(_WIN32)
void NTAPI TlsDestructor(void* value) {
#else
void TlsDestructor(void* value) {
#endif
  delete static_cast<int*>(value);
  g_tls_destructor_calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

TEST(ThreadLocalStorageTest, SlotGetSetWorksOnSameThread) {
  ThreadLocalStorage::Slot slot;
  EXPECT_TRUE(slot.Initialize());

  int value = 42;
  slot.Set(&value);
  EXPECT_EQ(slot.Get(), &value);

  slot.Set(nullptr);
  EXPECT_EQ(slot.Get(), nullptr);
}

TEST(ThreadLocalStorageTest, SlotIsThreadIsolated) {
  ThreadLocalStorage::Slot slot;
  EXPECT_TRUE(slot.Initialize());

  int main_value = 1;
  slot.Set(&main_value);

  void* worker_seen = reinterpret_cast<void*>(0x1);
  std::thread worker([&]() {
    worker_seen = slot.Get();
    int worker_value = 2;
    slot.Set(&worker_value);
    EXPECT_EQ(slot.Get(), &worker_value);
  });
  worker.join();

  EXPECT_EQ(worker_seen, nullptr);
  EXPECT_EQ(slot.Get(), &main_value);
}

TEST(ThreadLocalStorageTest, SlotDestructorRunsOnThreadExit) {
  g_tls_destructor_calls.store(0, std::memory_order_relaxed);

  ThreadLocalStorage::Slot slot(TlsDestructor);
  ASSERT_TRUE(slot.initialized());

  std::thread worker([&]() {
    slot.Set(new int(7));
    EXPECT_NE(slot.Get(), nullptr);
  });
  worker.join();

  EXPECT_EQ(g_tls_destructor_calls.load(std::memory_order_relaxed), 1);
}

}  // namespace nei
