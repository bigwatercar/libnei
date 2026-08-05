#include <gtest/gtest.h>

#include <vector>

#include <neixx/memory/memory_pressure_monitor.h>
#include <neixx/memory/small_object_allocator.h>

namespace {

class CollectingListener : public nei::MemoryPressureListener {
public:
  explicit CollectingListener(std::vector<nei::MemoryPressureLevel> *out)
      : out_(out) {}

  void OnMemoryPressure(nei::MemoryPressureLevel level) override {
    out_->push_back(level);
  }

private:
  std::vector<nei::MemoryPressureLevel> *out_;
};

} // namespace

TEST(MemoryPressureMonitorTest, LevelQueryReturnsValid) {
  const nei::MemoryPressureLevel level = nei::GetCurrentMemoryPressureLevel();
  EXPECT_TRUE(level == nei::MemoryPressureLevel::kNone ||
              level == nei::MemoryPressureLevel::kModerate ||
              level == nei::MemoryPressureLevel::kCritical);
}

TEST(MemoryPressureMonitorTest, ListenerRegistryNotifies) {
  std::vector<nei::MemoryPressureLevel> seen;
  CollectingListener listener(&seen);
  nei::MemoryPressureListenerRegistry::Add(&listener);

  nei::MemoryPressureListenerRegistry::Notify(nei::MemoryPressureLevel::kModerate);
  nei::MemoryPressureListenerRegistry::Notify(nei::MemoryPressureLevel::kCritical);
  nei::MemoryPressureListenerRegistry::Remove(&listener);

  EXPECT_EQ(seen.size(), 2u);
  ASSERT_GE(seen.size(), 2u);
  EXPECT_EQ(seen[0], nei::MemoryPressureLevel::kModerate);
  EXPECT_EQ(seen[1], nei::MemoryPressureLevel::kCritical);

  // After removal, no further notifications are delivered.
  nei::MemoryPressureListenerRegistry::Notify(nei::MemoryPressureLevel::kModerate);
  EXPECT_EQ(seen.size(), 2u);
}

TEST(MemoryPressureMonitorTest, MonitorPollOnceSamplesAndCaches) {
  std::vector<nei::MemoryPressureLevel> seen;
  CollectingListener listener(&seen);
  nei::MemoryPressureListenerRegistry::Add(&listener);
  nei::MemoryPressureMonitor monitor;

  const auto l1 = monitor.PollOnce();
  EXPECT_EQ(monitor.GetCurrentPressureLevel(), l1);
  // Any notifications fired must carry the sampled level.
  for (auto lv : seen) {
    EXPECT_EQ(lv, l1);
  }

  const auto l2 = monitor.PollOnce();
  EXPECT_EQ(monitor.GetCurrentPressureLevel(), l2);

  nei::MemoryPressureListenerRegistry::Remove(&listener);
}

// Demonstrates "主程序组装": the application wires a pressure listener that
// drives the small-object allocator's reclamation (Chromium:
// MemoryPressureMonitor -> PurgeMemory).  The monitor itself is decoupled from
// the allocator.
TEST(MemoryPressureMonitorTest, AssembleDrivesAllocatorReclamation) {
  using nei::SmallObjectAlloc;
  using nei::SmallObjectFree;
  using nei::PurgeSmallObjectAllocator;
  using nei::SmallObjectAllocatorStats;
  using nei::GetSmallObjectAllocatorStats;

  // Carve several chunks, then release every block so they sit idle.
  std::vector<void *> blocks;
  blocks.reserve(2000);
  for (int i = 0; i < 2000; ++i) {
    blocks.push_back(SmallObjectAlloc(32, 8));
  }
  for (void *p : blocks) {
    SmallObjectFree(p);
  }

  // The application's pressure listener: any non-NONE level triggers purge.
  struct PurgeListener : nei::MemoryPressureListener {
    void OnMemoryPressure(nei::MemoryPressureLevel level) override {
      if (level != nei::MemoryPressureLevel::kNone) {
        PurgeSmallObjectAllocator();
      }
    }
  };
  PurgeListener listener;
  nei::MemoryPressureListenerRegistry::Add(&listener);

  SmallObjectAllocatorStats before;
  GetSmallObjectAllocatorStats(&before);
  ASSERT_GT(before.committed_bytes, 0u);

  // Simulate the monitor detecting pressure (the application samples
  // GetCurrentMemoryPressureLevel() and calls Notify() on change).
  nei::MemoryPressureListenerRegistry::Notify(nei::MemoryPressureLevel::kCritical);

  SmallObjectAllocatorStats after;
  GetSmallObjectAllocatorStats(&after);
  EXPECT_GT(after.chunk_purges, before.chunk_purges);
  EXPECT_LT(after.committed_bytes, before.committed_bytes);

  nei::MemoryPressureListenerRegistry::Remove(&listener);
}
