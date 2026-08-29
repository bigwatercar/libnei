#pragma once
#ifndef NEIXX_MEMORY_SMALL_OBJECT_ALLOCATOR_H_
#define NEIXX_MEMORY_SMALL_OBJECT_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>

#include <nei/build/nei_export.h>

namespace nei {

// =============================================================================
// SmallObjectAllocator — a PartitionAlloc-inspired small-object allocator,
// usable by any component that allocates and frees many small objects (e.g.
// task/callback BindState).  All implementation details are private to the
// library; only the functions below are exported.
//
// Design highlights:
//   * Bucketed by size class; thread-local free lists (lock-free fast path)
//     with a mutex-protected central pool for cross-thread reuse.
//   * O(1) free via a 16-byte header stored immediately before every block.
//   * Direct (non-pooled) fallback for over-aligned (>16) or oversized
//     (>4096) requests, routed to plain ::operator new/delete.
//   * Chunks are page-backed (VirtualAlloc / mmap) and tracked per partition.
//     PurgeSmallObjectAllocator() decommits fully-free chunks (physical pages
//     returned to the OS, virtual address kept for cheap reuse); excess
//     decommitted chunks are released entirely.  Thread caches are shed via an
//     allocation counter — no internal timer or background thread.
//   * Freelist hardening: every free-list link is XOR-encoded with a per-
//     partition random key (fail-fast on corruption).
//   * Partition isolation: blocks never cross partitions; each partition owns
//     its own pool, freelist key, purge and stats.
//   * Memory-pressure awareness is provided by the independent
//     MemoryPressureMonitor component; applications drive reclamation by
//     calling PurgeSmallObjectAllocator() from a pressure listener.
//   * Committed-byte tracking and per-size-class stats for diagnostics.
//
// Thread safety: fully thread-safe.  Blocks allocated by any thread (or in any
// partition) may be freed by any thread; a partition must be destroyed only
// after all of its blocks have been freed and no thread is using it.
// =============================================================================

// ---- Default-partition API --------------------------------------------------

/// Allocates a block of at least `size` bytes aligned to `alignment` from the
/// default partition.  Throws bad_alloc if the underlying allocation fails.
NEI_API void *SmallObjectAlloc(std::size_t size,
                               std::size_t alignment = alignof(std::max_align_t));

/// Frees a block previously returned by SmallObjectAlloc (any partition).
/// No-op for null.
NEI_API void SmallObjectFree(void *ptr) noexcept;

/// Decommits fully-free chunks of the default partition back to the OS and
/// flushes the calling thread's cache.  Explicit, on-demand reclamation — no
/// internal timer.  Call it at memory-pressure or low-activity points (e.g.
/// from a MemoryPressureMonitor listener).
NEI_API void PurgeSmallObjectAllocator();

// ---- Diagnostics ------------------------------------------------------------

/// Diagnostic counters for stability/performance comparison.
struct SmallObjectAllocatorStats {
  std::uint64_t pooled_allocs;
  std::uint64_t pooled_frees;
  std::uint64_t direct_allocs;
  std::uint64_t chunk_allocs;
  std::uint64_t chunk_purges;
  std::uint64_t reserved_bytes;   // virtual address space kept by chunks
  std::uint64_t committed_bytes;  // physical memory actually committed
  std::uint64_t released_bytes;   // cumulative bytes returned to the OS entirely
};

/// Per-size-class breakdown for the default partition.
struct SmallObjectAllocatorSizeClassStats {
  std::uint64_t size;         // usable (returned) size of this class
  std::uint64_t in_use;       // blocks currently handed out
  std::uint64_t central_free; // free blocks in the central pool (sampled)
};

NEI_API void GetSmallObjectAllocatorStats(SmallObjectAllocatorStats *out);
NEI_API void ResetSmallObjectAllocatorStats();

/// Fills `out` with up to `capacity` per-size-class stats of the default
/// partition.  Returns the number of entries written (<= capacity).
NEI_API std::size_t GetSmallObjectAllocatorSizeClassStats(SmallObjectAllocatorSizeClassStats *out,
                                                          std::size_t capacity);

} // namespace nei

#endif // NEIXX_MEMORY_SMALL_OBJECT_ALLOCATOR_H_
