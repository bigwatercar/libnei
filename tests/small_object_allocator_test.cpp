#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <neixx/memory/small_object_allocator.h>

using nei::SmallObjectAlloc;
using nei::SmallObjectFree;
using nei::PurgeSmallObjectAllocator;
using nei::SmallObjectAllocatorStats;
using nei::GetSmallObjectAllocatorStats;
using nei::ResetSmallObjectAllocatorStats;
using nei::CreateSmallObjectAllocatorPartition;
using nei::DestroySmallObjectAllocatorPartition;
using nei::SmallObjectAllocInPartition;
using nei::PurgeSmallObjectAllocatorPartition;
using nei::GetSmallObjectAllocatorPartitionStats;
using nei::SmallObjectAllocatorSizeClassStats;
using nei::GetSmallObjectAllocatorSizeClassStats;

namespace {

// All blocks returned by the pooled path must honor the requested alignment
// and be usable for their full requested size.
void VerifyBlock(void *p, std::size_t size, std::size_t alignment) {
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignment, 0u);
  std::memset(p, 0x5A, size);
}

} // namespace

TEST(SmallObjectAllocatorTest, BasicAllocFreeRoundTrip) {
  // Sweep a few size classes: within the pool.
  const std::size_t sizes[] = {16, 32, 48, 64, 96, 128, 256, 512, 1024, 2048, 4096};
  std::vector<void *> blocks;
  for (std::size_t s : sizes) {
    void *p = nei::SmallObjectAlloc(s, 8);
    VerifyBlock(p, s, 8);
    blocks.push_back(p);
  }
  // Free in reverse order; should be a no-op-free / safe free.
  for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
    nei::SmallObjectFree(*it);
  }
  nei::SmallObjectFree(nullptr); // must be a no-op
}

TEST(SmallObjectAllocatorTest, OverAlignedAndOversizedGoDirect) {
  PurgeSmallObjectAllocator();
  ResetSmallObjectAllocatorStats();

  // 32-byte alignment on the pooled max size -> direct path.
  void *a = nei::SmallObjectAlloc(64, 32);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(a) % 32, 0u);

  // Oversized (> 4096) -> direct path.
  void *b = nei::SmallObjectAlloc(8192, 8);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b) % 8, 0u);

  SmallObjectAllocatorStats stats;
  GetSmallObjectAllocatorStats(&stats);
#if NEI_ALLOCATOR_DIAGNOSTICS
  EXPECT_GE(stats.direct_allocs, 2u);
#endif

  nei::SmallObjectFree(a);
  nei::SmallObjectFree(b);
}

TEST(SmallObjectAllocatorTest, PurgeReturnsFullyFreeChunks) {
  PurgeSmallObjectAllocator(); // clear leftovers from prior tests
  ResetSmallObjectAllocatorStats();

  constexpr int kCount = 2000; // enough to carve several 64-block chunks
  std::vector<void *> blocks;
  blocks.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    void *p = nei::SmallObjectAlloc(32, 8);
    ASSERT_NE(p, nullptr);
    std::memset(p, static_cast<int>(i & 0xFF), 32);
    blocks.push_back(p);
  }

  SmallObjectAllocatorStats before;
  GetSmallObjectAllocatorStats(&before);
  ASSERT_GT(before.committed_bytes, 0u);
  ASSERT_GE(before.committed_bytes, before.reserved_bytes); // all committed (or more if recycled)

  for (void *p : blocks) {
    nei::SmallObjectFree(p);
  }

  const std::uint64_t committed_before = before.committed_bytes;
  const std::uint64_t reserved_before = before.reserved_bytes;
  const std::uint64_t purges_before = before.chunk_purges;
  const std::uint64_t released_before = before.released_bytes;

  PurgeSmallObjectAllocator();

  SmallObjectAllocatorStats after;
  GetSmallObjectAllocatorStats(&after);
  // Every chunk was idle, so the purge decommits them all: physical pages are
  // returned to the OS.  A small set per class is kept (decommitted) for cheap
  // reuse; the rest are released entirely to bound virtual address growth.
  EXPECT_GT(after.chunk_purges, purges_before);
  EXPECT_EQ(after.committed_bytes, 0u);
  EXPECT_LE(after.reserved_bytes, reserved_before);
  EXPECT_GT(after.released_bytes, released_before);
}

TEST(SmallObjectAllocatorTest, AllocatorStillUsableAfterPurge) {
  PurgeSmallObjectAllocator();

  // Allocate/free repeatedly after a purge; the allocator must re-commit
  // decommitted chunks and remain correct (no use-after-free).
  for (int round = 0; round < 4; ++round) {
    std::vector<void *> blocks;
    for (int i = 0; i < 500; ++i) {
      void *p = nei::SmallObjectAlloc(48, 8);
      ASSERT_NE(p, nullptr);
      std::memset(p, static_cast<int>(round * 31 + i) & 0xFF, 48);
      blocks.push_back(p);
    }
    for (void *p : blocks) {
      nei::SmallObjectFree(p);
    }
    PurgeSmallObjectAllocator();
  }
}

TEST(SmallObjectAllocatorTest, DecommittedChunkReused) {
  PurgeSmallObjectAllocator();
  ResetSmallObjectAllocatorStats();

  // Carve several chunks of size-32 blocks, then release every block.
  constexpr int kCount = 1000;
  std::vector<void *> blocks;
  blocks.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    blocks.push_back(nei::SmallObjectAlloc(32, 8));
  }
  for (void *p : blocks) {
    nei::SmallObjectFree(p);
  }
  PurgeSmallObjectAllocator(); // decommits idle chunks (keeps up to 4 per class)

  SmallObjectAllocatorStats after_purge;
  GetSmallObjectAllocatorStats(&after_purge);
  ASSERT_GT(after_purge.chunk_allocs, 0u);
  ASSERT_EQ(after_purge.committed_bytes, 0u);

  // Re-allocate a small batch (<= 4 decommitted chunks * 64 = 256 blocks):
  // must reuse parked decommitted chunks rather than carving new ones.
  std::vector<void *> blocks2;
  constexpr int kReuse = 200;
  blocks2.reserve(kReuse);
  for (int i = 0; i < kReuse; ++i) {
    blocks2.push_back(nei::SmallObjectAlloc(32, 8));
  }
  SmallObjectAllocatorStats reused;
  GetSmallObjectAllocatorStats(&reused);
  EXPECT_EQ(reused.chunk_allocs, after_purge.chunk_allocs); // no new chunks
  EXPECT_GT(reused.committed_bytes, after_purge.committed_bytes); // recommitted

  for (void *p : blocks2) {
    nei::SmallObjectFree(p);
  }
}

TEST(SmallObjectAllocatorTest, ConcurrentAllocFreeWithPurgeNoCrash) {
  constexpr int kThreads = 4;
  constexpr int kIters = 20000;
  std::atomic<bool> stop{false};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([t, &stop]() {
      std::vector<void *> scratch;
      scratch.reserve(256);
      std::uint32_t seed = static_cast<std::uint32_t>(t) * 2654435761u + 1u;
      for (int iter = 0; iter < kIters && !stop.load(std::memory_order_relaxed);
           ++iter) {
        // Pick a small size class pseudo-randomly.
        const std::size_t sizes[] = {16, 32, 48, 64, 96, 128};
        const std::size_t s = sizes[seed % (sizeof(sizes) / sizeof(sizes[0]))];
        seed = seed * 1664525u + 1013904223u;
        void *p = nei::SmallObjectAlloc(s, 8);
        if (p == nullptr) {
          continue;
        }
        std::memset(p, static_cast<int>(seed & 0xFF), s);
        scratch.push_back(p);
        if (scratch.size() >= 128) {
          for (void *q : scratch) {
            nei::SmallObjectFree(q);
          }
          scratch.clear();
        }
      }
      for (void *q : scratch) {
        nei::SmallObjectFree(q);
      }
    });
  }

  // Main thread drives purges concurrently with the workers.
  for (int i = 0; i < 40; ++i) {
    PurgeSmallObjectAllocator();
    std::this_thread::yield();
  }
  stop.store(true, std::memory_order_relaxed);
  for (auto &w : workers) {
    w.join();
  }

  // One final purge must be safe.
  PurgeSmallObjectAllocator();
}

TEST(SmallObjectAllocatorTest, PartitionIsolation) {
  PurgeSmallObjectAllocator();
  ResetSmallObjectAllocatorStats();

  auto *part = CreateSmallObjectAllocatorPartition();
  ASSERT_NE(part, nullptr);

  // Allocate one block in the default partition and one in the new partition.
  void *a = SmallObjectAlloc(32, 8);
  void *b = SmallObjectAllocInPartition(part, 32, 8);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  std::memset(a, 0x11, 32);
  std::memset(b, 0x22, 32);

  SmallObjectAllocatorStats def_stats;
  GetSmallObjectAllocatorStats(&def_stats);
  SmallObjectAllocatorStats part_stats;
  GetSmallObjectAllocatorPartitionStats(part, &part_stats);
#if NEI_ALLOCATOR_DIAGNOSTICS
  EXPECT_GE(def_stats.pooled_allocs, 1u);
  EXPECT_GE(part_stats.pooled_allocs, 1u);
#endif

  // Free must route back to the correct partition via the block header.
  SmallObjectFree(a);
  SmallObjectFree(b);

  PurgeSmallObjectAllocatorPartition(part);
  DestroySmallObjectAllocatorPartition(part);
}

TEST(SmallObjectAllocatorTest, PartitionExhaustionReturnsNull) {
  // Allocate user partitions until the fixed budget is exhausted.
  std::vector<nei::SmallObjectAllocatorPartition *> parts;
  while (auto *p = CreateSmallObjectAllocatorPartition()) {
    parts.push_back(p);
  }
  EXPECT_NE(parts.size(), 0u);
  // At least one attempt must have failed.
  EXPECT_EQ(CreateSmallObjectAllocatorPartition(), nullptr);

  for (auto *p : parts) {
    DestroySmallObjectAllocatorPartition(p);
  }
}

TEST(SmallObjectAllocatorTest, SizeClassStatsReport) {
  PurgeSmallObjectAllocator();
  ResetSmallObjectAllocatorStats();

  std::vector<void *> blocks;
  for (int i = 0; i < 10; ++i) {
    blocks.push_back(SmallObjectAlloc(48, 8)); // size class 48
  }

  SmallObjectAllocatorSizeClassStats buf[32];
  const std::size_t n = GetSmallObjectAllocatorSizeClassStats(buf, 32);
  ASSERT_EQ(n, 21u); // all 21 size classes reported

  bool found_48 = false;
  for (std::size_t i = 0; i < n; ++i) {
    if (buf[i].size == 48) {
      found_48 = true;
#if NEI_ALLOCATOR_DIAGNOSTICS
      EXPECT_EQ(buf[i].in_use, 10u);
#else
      EXPECT_EQ(buf[i].in_use, 0u);
#endif
    }
  }
  EXPECT_TRUE(found_48);

  for (void *p : blocks) {
    SmallObjectFree(p);
  }
}
