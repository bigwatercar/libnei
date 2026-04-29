#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include <neixx/threading/thread_local_storage.h>

namespace nei {
namespace {

std::atomic<int> *g_tls_destructor_counter = nullptr;

void CountAndDeleteInt(void *value) {
  if (value != nullptr) {
    delete static_cast<int *>(value);
  }
  if (g_tls_destructor_counter != nullptr) {
    g_tls_destructor_counter->fetch_add(1, std::memory_order_relaxed);
  }
}

TEST(ThreadLocalStorageTest, SlotSetAndGetOnCurrentThread) {
  ThreadLocalStorage::Slot slot;
  ASSERT_TRUE(slot.IsValid());

  int value = 42;
  slot.Set(&value);
  EXPECT_EQ(slot.Get(), &value);

  slot.Set(nullptr);
  EXPECT_EQ(slot.Get(), nullptr);
}

TEST(ThreadLocalStorageTest, SlotValuesAreIsolatedPerThread) {
  ThreadLocalStorage::Slot slot;
  ASSERT_TRUE(slot.IsValid());

  int main_value = 7;
  slot.Set(&main_value);

  void *worker_seen = reinterpret_cast<void *>(1);
  int worker_value = 9;
  std::thread worker([&]() {
    worker_seen = slot.Get();
    slot.Set(&worker_value);
    EXPECT_EQ(slot.Get(), &worker_value);
  });
  worker.join();

  EXPECT_EQ(worker_seen, nullptr);
  EXPECT_EQ(slot.Get(), &main_value);
}

TEST(ThreadLocalStorageTest, DestructorRunsOnThreadExit) {
  std::atomic<int> destroyed_count{0};
  g_tls_destructor_counter = &destroyed_count;

  {
    ThreadLocalStorage::Slot slot(&CountAndDeleteInt);
    ASSERT_TRUE(slot.IsValid());

    std::thread worker([&]() {
      slot.Set(new int(123));
      EXPECT_NE(slot.Get(), nullptr);
    });
    worker.join();
  }

  EXPECT_EQ(destroyed_count.load(std::memory_order_relaxed), 1);
  g_tls_destructor_counter = nullptr;
}

} // namespace
} // namespace nei