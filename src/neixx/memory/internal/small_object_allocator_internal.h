#pragma once
#ifndef NEIXX_MEMORY_SRC_INTERNAL_SMALL_OBJECT_ALLOCATOR_INTERNAL_H_
#define NEIXX_MEMORY_SRC_INTERNAL_SMALL_OBJECT_ALLOCATOR_INTERNAL_H_

// =============================================================================
// Internal API — partition isolation and per-partition diagnostics.
// NOT part of the public ABI.  Exposed only for testing.
// =============================================================================

#include <cstddef>
#include <cstdint>

#include <nei/build/nei_export.h>

namespace nei {

struct SmallObjectAllocatorPartition;
struct SmallObjectAllocatorStats;
struct SmallObjectAllocatorSizeClassStats;

// ---- Partition isolation (test-only) ---------------------------------------

/// Creates a new isolated partition (up to 7 beyond the default).
/// Returns nullptr if the partition limit is reached.
NEI_API SmallObjectAllocatorPartition *CreateSmallObjectAllocatorPartition();

/// Destroys a partition.  The caller must have freed every block allocated in
/// it and stopped using it beforehand.
NEI_API void DestroySmallObjectAllocatorPartition(SmallObjectAllocatorPartition *partition);

/// Allocates from a specific partition (or the default if partition is null).
NEI_API void *SmallObjectAllocInPartition(SmallObjectAllocatorPartition *partition,
                                          std::size_t size,
                                          std::size_t alignment = alignof(std::max_align_t));

/// Decommits fully-free chunks of a specific partition.
NEI_API void PurgeSmallObjectAllocatorPartition(SmallObjectAllocatorPartition *partition);

// ---- Per-partition diagnostics (test-only) ---------------------------------

NEI_API void GetSmallObjectAllocatorPartitionStats(SmallObjectAllocatorPartition *partition,
                                                   SmallObjectAllocatorStats *out);

NEI_API std::size_t GetSmallObjectAllocatorPartitionSizeClassStats(
    SmallObjectAllocatorPartition *partition,
    SmallObjectAllocatorSizeClassStats *out,
    std::size_t capacity);

} // namespace nei

#endif // NEIXX_MEMORY_SRC_INTERNAL_SMALL_OBJECT_ALLOCATOR_INTERNAL_H_
