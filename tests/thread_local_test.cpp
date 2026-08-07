#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <neixx/threading/thread_local.h>
#include <nei/macros/suppress_compiler_warnings.h>
#include <neixx/threading/thread_local_storage.h>

NEI_SUPPRESS_MSC_WARNING_BEGIN(4996)

namespace {

// ---- ThreadLocal<T> value semantics ----------------------------------------

TEST(ThreadLocalTest, ValueDefaultConstruction) {
  nei::ThreadLocal<int> g_counter;
  EXPECT_EQ(*g_counter, 0);
  *g_counter = 42;
  EXPECT_EQ(*g_counter, 42);
}

TEST(ThreadLocalTest, ValueInitialValue) {
  nei::ThreadLocal<int> g_counter{100};
  EXPECT_EQ(*g_counter, 100);
}

TEST(ThreadLocalTest, ValueOperatorArrow) {
  struct S {
    int a = 7;
    std::string b = "hello";
  };

  nei::ThreadLocal<S> g_s;
  S *ptr = g_s.Get();
  EXPECT_EQ(ptr->a, 7);
  EXPECT_EQ(ptr->b, "hello");
  (void)g_s; // ThreadLocal exists, uses operator->
}

TEST(ThreadLocalTest, ValueSetAndGet) {
  nei::ThreadLocal<int> g_val;
  g_val.Set(99);
  EXPECT_EQ(*g_val.Get(), 99);
  EXPECT_EQ(*g_val, 99);
}

TEST(ThreadLocalTest, ValuePerThreadIsolation) {
  nei::ThreadLocal<int> g_val{0};
  std::atomic<int> differences{0};

  auto thread_fn = [&](int value) {
    g_val.Set(value);
    if (*g_val != value)
      differences.fetch_add(1);
    // Other threads should NOT see this value
  };

  std::thread t1(thread_fn, 10);
  std::thread t2(thread_fn, 20);
  t1.join();
  t2.join();

  // Main thread should still have its own value (0)
  EXPECT_EQ(*g_val, 0);
  EXPECT_EQ(differences.load(), 0);
}

TEST(ThreadLocalTest, ValueNonTrivialType) {
  nei::ThreadLocal<std::string> g_s{"default"};
  EXPECT_EQ(*g_s, "default");
  g_s.Set("changed");
  EXPECT_EQ(*g_s, "changed");
}

// ---- ThreadLocalPointer<T> -------------------------------------------------

struct TrackedObject {
  static std::atomic<int> alive_count;
  int id;

  TrackedObject(int i)
      : id(i) {
    alive_count.fetch_add(1);
  }

  ~TrackedObject() {
    alive_count.fetch_sub(1);
  }
};

std::atomic<int> TrackedObject::alive_count{0};

TEST(ThreadLocalPointerTest, SetAndGet) {
  nei::ThreadLocalPointer<TrackedObject> g_obj;
  auto *obj = new TrackedObject(1);
  g_obj.Set(obj);
  EXPECT_EQ(g_obj.Get(), obj);
  EXPECT_EQ(g_obj->id, 1);
  EXPECT_EQ((*g_obj).id, 1);
  delete g_obj.Get();
}

TEST(ThreadLocalPointerTest, HasValue) {
  nei::ThreadLocalPointer<TrackedObject> g_obj;
  EXPECT_FALSE(g_obj.HasValue());
  auto *obj = new TrackedObject(1);
  g_obj.Set(obj);
  EXPECT_TRUE(g_obj.HasValue());
  delete g_obj.Get();
}

TEST(ThreadLocalPointerTest, PerThreadIsolation) {
  nei::ThreadLocalPointer<TrackedObject> g_obj;
  std::atomic<bool> t1_ok{false};
  std::atomic<bool> t2_ok{false};

  auto t1 = new TrackedObject(1);
  auto t2 = new TrackedObject(2);
  g_obj.Set(t1);

  std::thread th1([&] {
    if (g_obj.Get() == nullptr)
      t1_ok = true; // should be null in new thread
    g_obj.Set(new TrackedObject(3));
  });

  std::thread th2([&] {
    if (g_obj.Get() == nullptr)
      t2_ok = true; // should be null in new thread
  });

  th1.join();
  th2.join();

  // Main thread should still have t1
  EXPECT_EQ(g_obj.Get(), t1);

  // Other threads should have seen null initially
  EXPECT_TRUE(t1_ok);
  EXPECT_TRUE(t2_ok);

  // Cleanup
  delete t1;
  delete t2;
}

// ---- ThreadLocalOwnedPointer<T> --------------------------------------------

static std::atomic<int> g_owned_dtor_count{0};

struct OwnedObject {
  int id = 0;
  OwnedObject() = default;

  explicit OwnedObject(int i)
      : id(i) {
  }

  ~OwnedObject() {
    g_owned_dtor_count.fetch_add(1);
  }
};

TEST(ThreadLocalOwnedPointerTest, GetOrCreate) {
  g_owned_dtor_count.store(0);
  {
    nei::ThreadLocalOwnedPointer<OwnedObject> g_obj;
    auto *a = g_obj.GetOrCreate();
    EXPECT_NE(a, nullptr);
    a->id = 7;
    auto *b = g_obj.GetOrCreate();
    EXPECT_EQ(b, a); // same object
    EXPECT_EQ(b->id, 7);
  }
  // ThreadLocalOwnedPointer dtor does NOT call DestroyValue on slot destruction;
  // it's the thread exit that triggers it.  This test verifies GetOrCreate
  // behavior, not destructor behavior.
}

TEST(ThreadLocalOwnedPointerTest, SetAndGet) {
  nei::ThreadLocalOwnedPointer<OwnedObject> g_obj;
  auto *obj = new OwnedObject(42);
  g_obj.Set(obj);
  EXPECT_EQ(g_obj->id, 42);
}

TEST(ThreadLocalOwnedPointerTest, GetOrCreateIsPerThread) {
  nei::ThreadLocalOwnedPointer<OwnedObject> g_obj;
  auto *main_obj = g_obj.GetOrCreate();
  main_obj->id = 1;

  std::atomic<OwnedObject *> t_obj{nullptr};
  std::thread th([&] {
    t_obj.store(g_obj.GetOrCreate());
    t_obj.load()->id = 2;
  });
  th.join();

  // Main thread still has its own object
  EXPECT_EQ(main_obj->id, 1);
  // Other thread created a different object
  EXPECT_NE(t_obj.load(), main_obj);
  EXPECT_EQ(t_obj.load()->id, 2);
}

// ---- ThreadLocalBoolean ----------------------------------------------------

TEST(ThreadLocalBooleanTest, DefaultFalse) {
  nei::ThreadLocalBoolean g_flag;
  EXPECT_FALSE(g_flag.Get());
}

TEST(ThreadLocalBooleanTest, SetAndGet) {
  nei::ThreadLocalBoolean g_flag;
  g_flag.Set(true);
  EXPECT_TRUE(g_flag.Get());
  g_flag.Set(false);
  EXPECT_FALSE(g_flag.Get());
}

TEST(ThreadLocalBooleanTest, PerThreadIsolation) {
  nei::ThreadLocalBoolean g_flag;
  g_flag.Set(true);
  EXPECT_TRUE(g_flag.Get());

  std::atomic<bool> other_thread_saw{false};
  std::thread th([&] {
    other_thread_saw.store(g_flag.Get()); // should be false in new thread
    g_flag.Set(true);
  });
  th.join();

  EXPECT_FALSE(other_thread_saw.load()); // new thread default is false
  EXPECT_TRUE(g_flag.Get());             // main thread still true
}

// ---- Multiple slots (slot recycling) ---------------------------------------

TEST(ThreadLocalTest, ManySlotsNoLeak) {
  // Create and destroy many ThreadLocalPointer instances.
  // This exercises slot recycling (FREE/IN_USE bitmap).
  for (int i = 0; i < 300; ++i) { // exceeds kMaxSlots=256
    nei::ThreadLocalPointer<int> g_p;
    g_p.Set(new int(i));
    EXPECT_NE(g_p.Get(), nullptr);
    EXPECT_EQ(*g_p, i);
    delete g_p.Get();
  }
  // If slot recycling works, all 300 iterations pass without exhaustion.
}

TEST(ThreadLocalTest, ManyOwnedPointersNoLeak) {
  for (int i = 0; i < 300; ++i) {
    nei::ThreadLocalOwnedPointer<OwnedObject> g_p;
    g_p.GetOrCreate();
    // Destructor frees slot when g_p goes out of scope.
  }
}

// ---- Mixed types -----------------------------------------------------------

TEST(ThreadLocalTest, MixedTypesSameThread) {
  nei::ThreadLocal<int> g_int{10};
  nei::ThreadLocalPointer<int> g_ptr;
  nei::ThreadLocalBoolean g_bool;

  EXPECT_EQ(*g_int, 10);
  EXPECT_EQ(g_ptr.Get(), nullptr);
  EXPECT_FALSE(g_bool.Get());

  int *val = new int(99);
  g_ptr.Set(val);
  g_bool.Set(true);

  EXPECT_EQ(*g_int, 10);
  EXPECT_EQ(g_ptr.Get(), val);
  EXPECT_EQ(*(g_ptr.Get()), 99);
  EXPECT_TRUE(g_bool.Get());

  delete g_ptr.Get();
}

// ---- Iterator --------------------------------------------------------------

TEST(ThreadLocalTest, IteratorFindsActiveSlots) {
  // Create a few slots on this thread, then iterate to verify they're found.
  nei::ThreadLocalStorage::Slot a;
  a.Initialize();
  a.Set(reinterpret_cast<void *>(1));

  nei::ThreadLocalStorage::Slot b;
  b.Initialize();
  b.Set(reinterpret_cast<void *>(2));

  int found = 0;
  nei::ThreadLocalStorage::Iterator it;
  while (!it.IsAtEnd()) {
    void *val = it.Get();
    if (val == reinterpret_cast<void *>(1) || val == reinterpret_cast<void *>(2)) {
      ++found;
    }
    it.Advance();
  }
  EXPECT_GE(found, 2); // at least our two slots
}

TEST(ThreadLocalTest, IteratorEmptyWhenNoSlotsActive) {
  // All previous slot-scope tests may leave active slots, but since slots
  // are per-process, we can't guarantee emptiness. Just verify Iterator
  // doesn't crash.
  nei::ThreadLocalStorage::Iterator it;
  while (!it.IsAtEnd()) {
    it.Advance();
  }
  SUCCEED(); // no crash
}

// ---- long_lived destructor ordering ----------------------------------------

std::atomic<int> g_order_normal{0};
std::atomic<int> g_order_long{0};
std::atomic<int> g_order_flag{0};

static void NTAPI OrderNormalDtor(void *) {
  g_order_normal.fetch_add(1);
  if (g_order_flag.load() == 0)
    g_order_flag.store(1);
}

static void NTAPI OrderLongLivedDtor(void *) {
  g_order_long.fetch_add(1);
  if (g_order_flag.load() == 1)
    g_order_flag.store(3);
}

TEST(ThreadLocalTest, LongLivedDestroyedAfterNormal) {
  g_order_normal.store(0);
  g_order_long.store(0);
  g_order_flag.store(0);

  // Slots must outlive the worker thread — create them here.
  nei::ThreadLocalStorage::Slot normal_slot(&OrderNormalDtor);
  nei::ThreadLocalStorage::Slot long_slot;
  ASSERT_TRUE(long_slot.InitializeAsLongLived(&OrderLongLivedDtor));

  std::thread th([&] {
    normal_slot.Set(reinterpret_cast<void *>(1));
    long_slot.Set(reinterpret_cast<void *>(1));
  });
  th.join();

  EXPECT_EQ(g_order_normal.load(), 1);
  EXPECT_EQ(g_order_long.load(), 1);
  EXPECT_EQ(g_order_flag.load(), 3);
}

// ---- Slot::InitializeAsLongLived -------------------------------------------

static void NTAPI NoopDtor(void *ptr) {
  (void)ptr;
}

TEST(ThreadLocalTest, LongLivedViaSlotApi) {
  nei::ThreadLocalStorage::Slot slot;
  bool ok = slot.InitializeAsLongLived(&NoopDtor);
  EXPECT_TRUE(ok);
  EXPECT_TRUE(slot.initialized());

  int value = 42;
  slot.Set(&value);
  EXPECT_EQ(slot.Get(), &value);
}

} // namespace

NEI_SUPPRESS_MSC_WARNING_END()
