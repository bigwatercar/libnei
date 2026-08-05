// small_object_allocator.cpp — implementation of the PartitionAlloc-inspired
// small-object allocator.  All details (size classes, thread cache, central
// pools, page-backed chunks, freelist encoding, purge, partitions) are private
// to this translation unit; only the exported functions declared in
// small_object_allocator.h are visible to consumers.

#include <neixx/memory/small_object_allocator.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON // macOS
#endif
#endif

namespace nei {

// ---- Shared constants and types (visible to the exported API below) ---------

// Usable (returned) sizes.  All multiples of 16 so a block base + the 16-byte
// header keeps the returned pointer 16-aligned (== max_align on x64).
constexpr std::size_t kUsableSizes[] = {
    16,  32,  48,  64,  96,   128,  160,  192,  256,  320,  384,
    512, 640, 768, 1024, 1280, 1536, 2048, 2560, 3072, 4096,
};
constexpr std::size_t kNumSizeClasses = sizeof(kUsableSizes) / sizeof(kUsableSizes[0]);

struct Chunk;

// 16-byte header immediately before every returned pointer.  The free-list link
// overwrites bytes 0..7 while a block is in a free list; `chunk_or_raw`
// (bytes 8..15) is preserved so the owning chunk is always recoverable in O(1).
struct BlockHeader {
  std::uint32_t magic;
  std::uint32_t size_class;
  void *chunk_or_raw; // Pooled: Chunk*. Direct: ::operator new base.
};

// One isolated allocator partition (same type as the public opaque handle).
// `m` guards heads/chunks/decommitted lists (and the per-chunk `central_count`).
// All other fields are atomics updated lock-free on the hot path.
struct SmallObjectAllocatorPartition {
  std::mutex m;
  std::uint32_t index = 0;
  std::uintptr_t freelist_key = 0;
  void *heads[kNumSizeClasses] = {};
  Chunk *chunks = nullptr;                        // committed chunks (guarded by m)
  Chunk *decommitted_heads[kNumSizeClasses] = {}; // reusable decommitted chunks (guarded by m)
  std::atomic<std::uint64_t> pooled_allocs{0};
  std::atomic<std::uint64_t> pooled_frees{0};
  std::atomic<std::uint64_t> direct_allocs{0};
  std::atomic<std::uint64_t> chunk_allocs{0};
  std::atomic<std::uint64_t> chunk_purges{0};
  std::atomic<std::uint64_t> reserved_bytes{0};
  std::atomic<std::uint64_t> committed_bytes{0};
  std::atomic<std::uint64_t> released_bytes{0};
  std::atomic<std::uint64_t> in_use_by_class[kNumSizeClasses]{};
};

// A page-backed region of kBatchSize same-size-class blocks.  Accounting:
//   * `in_use`        — atomic, updated lock-free on the hot path.  Number of
//                       blocks currently handed out to callers.
//   * `central_count` — blocks currently in the central free list for this
//                       chunk.  Only mutated under p->m, so it is authoritative
//                       for any purge that holds that lock.
// A chunk is decommit-eligible iff `central_count == kBatchSize && in_use == 0`
// (every block idle in the central pool).  Because a chunk can only become
// eligible through a lock-held flush of its last block to central (serialized
// with purge), the check is race-free even though `in_use` is lock-free.
struct Chunk {
  char *base;
  std::uint32_t size_class;
  std::uint32_t stride;
  std::atomic<std::uint32_t> in_use{0};
  std::uint32_t central_count = 0;
  bool committed = true;
  SmallObjectAllocatorPartition *partition = nullptr;
  Chunk *next = nullptr;             // committed-chunk list
  Chunk *decommitted_next = nullptr; // decommitted reuse list
};

namespace {

constexpr std::uint32_t kPooledMagic = 0xCA11CA00u;
constexpr std::uint32_t kDirectMagic = 0xCA11CA01u;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kMaxAlign = 16;
constexpr std::size_t kMaxPooledSize = 4096;
constexpr std::size_t kBatchSize = 64;
constexpr std::size_t kThreadCacheCap = 256;
// Counter-based self-purge: every kAllocBatch allocations, shed half of the
// current thread's per-class cache back to the central pool (Chromium-style,
// no internal timer).
constexpr std::size_t kAllocBatch = 1024;
// Fixed partition budget: index 0 is the default partition, 1..kMaxPartitions-1
// are user partitions.
constexpr std::size_t kMaxPartitions = 8;
// How many decommitted chunks per size class are kept for cheap reuse before
// older ones are released entirely (bounds virtual address growth).
constexpr std::size_t kMaxDecommittedPerClass = 4;

// Partition registry: index 0 is the default partition; 1..kMaxPartitions-1 are
// user partitions.  g_num_created counts created partitions (>= 1).
std::atomic<std::uint32_t> g_num_created{1};
std::atomic<SmallObjectAllocatorPartition *> g_partitions[kMaxPartitions]{};

std::size_t IndexForSize(std::size_t size) {
  for (std::size_t i = 0; i < kNumSizeClasses; ++i) {
    if (size <= kUsableSizes[i]) {
      return i;
    }
  }
  return kNumSizeClasses; // too large -> direct
}

std::uintptr_t GenerateFreelistKey() {
  static const auto epoch =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::uintptr_t key = static_cast<std::uintptr_t>(epoch);
  key ^= reinterpret_cast<std::uintptr_t>(&key);
  key ^= static_cast<std::uintptr_t>(std::uint32_t(key >> 32));
  if (key == 0) {
    key = 0x9E3779B97F4A7C15ull; // gold ratio constant as a fallback
  }
  return key;
}

SmallObjectAllocatorPartition &DefaultPartition() {
  static SmallObjectAllocatorPartition *p = [] {
    auto *q = new SmallObjectAllocatorPartition;
    q->index = 0;
    q->freelist_key = GenerateFreelistKey();
    g_partitions[0].store(q, std::memory_order_relaxed);
    return q;
  }();
  return *p;
}

// ---- Page allocator (VirtualAlloc / mmap) ----------------------------------
void *PageAlloc(std::size_t bytes) {
#if defined(_WIN32)
  return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void *p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? nullptr : p;
#endif
}

void PageFree(void *ptr, std::size_t bytes) {
#if defined(_WIN32)
  (void)bytes;
  VirtualFree(ptr, 0, MEM_RELEASE);
#else
  ::munmap(ptr, bytes);
#endif
}

// Return physical pages to the OS while keeping the virtual address range
// (DiscardSystemPages semantics).
void PageDiscard(void *ptr, std::size_t bytes) {
#if defined(_WIN32)
  (void)bytes;
  VirtualFree(ptr, bytes, MEM_DECOMMIT);
#else
  ::madvise(ptr, bytes, MADV_DONTNEED);
#endif
}

// Re-commit pages previously handed back by PageDiscard.  On POSIX a
// MADV_DONTNEED'd mapping remains valid and faults in zero pages, so this is a
// no-op; on Windows the range must be committed again before it is accessible.
void PageRecommit(void *ptr, std::size_t bytes) {
#if defined(_WIN32)
  VirtualAlloc(ptr, bytes, MEM_COMMIT, PAGE_READWRITE);
#else
  (void)ptr;
  (void)bytes;
#endif
}

// ---- Freelist hardening (XOR encoding) -------------------------------------
std::uintptr_t FreelistEncode(SmallObjectAllocatorPartition *p, void *ptr) {
  return reinterpret_cast<std::uintptr_t>(ptr) ^ p->freelist_key;
}

void *FreelistDecode(SmallObjectAllocatorPartition *p, std::uintptr_t encoded) {
  return reinterpret_cast<void *>(encoded ^ p->freelist_key);
}

Chunk *ChunkOf(void *block) {
  return *reinterpret_cast<Chunk **>(static_cast<char *>(block) + 8);
}

// ---- Thread cache (lock-free fast path, per thread per partition) -----------
struct ThreadCache {
  void *heads[kNumSizeClasses];
  std::uint32_t counts[kNumSizeClasses];
  std::uint32_t alloc_count = 0; // counter for self-purge
};
thread_local ThreadCache g_tls_cache[kMaxPartitions]{};

// Pushes a block into the thread cache.  Only bytes 0..7 (the encoded free-list
// link) are overwritten; bytes 8..15 keep the owning chunk pointer.
void ThreadCachePush(SmallObjectAllocatorPartition *p, std::size_t idx, void *block) {
  ThreadCache &tc = g_tls_cache[p->index];
  *reinterpret_cast<std::uintptr_t *>(block) = FreelistEncode(p, tc.heads[idx]);
  tc.heads[idx] = block;
  ++tc.counts[idx];
}

// Pops a block from the thread cache.
void *ThreadCachePop(SmallObjectAllocatorPartition *p, std::size_t idx) {
  ThreadCache &tc = g_tls_cache[p->index];
  void *block = tc.heads[idx];
  if (block != nullptr) {
    tc.heads[idx] = FreelistDecode(p, *reinterpret_cast<std::uintptr_t *>(block));
    --tc.counts[idx];
  }
  return block;
}

void *CentralPop(SmallObjectAllocatorPartition *p, std::size_t idx) {
  std::lock_guard<std::mutex> lock(p->m);
  void *block = p->heads[idx];
  if (block != nullptr) {
    p->heads[idx] = FreelistDecode(p, *reinterpret_cast<std::uintptr_t *>(block));
    ChunkOf(block)->central_count -= 1;
  }
  return block;
}

// Move `n` blocks from the current thread cache to the central pool.
void FlushToCentral(SmallObjectAllocatorPartition *p, std::size_t idx, std::uint32_t n) {
  std::lock_guard<std::mutex> lock(p->m);
  for (std::uint32_t i = 0; i < n; ++i) {
    void *block = ThreadCachePop(p, idx);
    if (block == nullptr) {
      break;
    }
    ChunkOf(block)->central_count += 1;
    *reinterpret_cast<std::uintptr_t *>(block) = FreelistEncode(p, p->heads[idx]);
    p->heads[idx] = block;
  }
}

// Move all blocks of the current thread cache for `idx` to central.
void FlushAllToCentral(SmallObjectAllocatorPartition *p, std::size_t idx) {
  while (g_tls_cache[p->index].counts[idx] > 0) {
    FlushToCentral(p, idx, g_tls_cache[p->index].counts[idx]);
  }
}

// Obtains a committed chunk for `idx`: prefers a decommitted chunk parked in the
// reuse list (recommit + reset), otherwise carves a fresh page-backed chunk.
// Called under p->m (decommitted list and chunk list are guarded by it).
Chunk *AcquireChunk(SmallObjectAllocatorPartition *p, std::size_t idx) {
  const std::size_t usable = kUsableSizes[idx];
  const std::size_t stride = usable + kHeaderSize;
  const std::size_t chunk_bytes = stride * kBatchSize;
  std::lock_guard<std::mutex> lock(p->m);

  Chunk *c = p->decommitted_heads[idx];
  if (c != nullptr) {
    p->decommitted_heads[idx] = c->decommitted_next;
    PageRecommit(c->base, chunk_bytes);
    c->committed = true;
    c->central_count = 0;
    c->in_use.store(0, std::memory_order_relaxed);
    c->partition = p;
    c->next = p->chunks;
    p->chunks = c;
    p->committed_bytes.fetch_add(chunk_bytes, std::memory_order_relaxed);
    return c;
  }

  void *base = PageAlloc(chunk_bytes);
  if (base == nullptr) {
    return nullptr;
  }
  auto *chunk = new Chunk;
  chunk->base = static_cast<char *>(base);
  chunk->size_class = static_cast<std::uint32_t>(idx);
  chunk->stride = static_cast<std::uint32_t>(stride);
  chunk->partition = p;
  chunk->committed = true;
  chunk->next = p->chunks;
  p->chunks = chunk;
  p->chunk_allocs.fetch_add(1, std::memory_order_relaxed);
  p->reserved_bytes.fetch_add(chunk_bytes, std::memory_order_relaxed);
  p->committed_bytes.fetch_add(chunk_bytes, std::memory_order_relaxed);
  return chunk;
}

void *Refill(SmallObjectAllocatorPartition *p, std::size_t idx) {
  void *block = CentralPop(p, idx);
  if (block != nullptr) {
    return block;
  }
  Chunk *chunk = AcquireChunk(p, idx);
  if (chunk == nullptr) {
    return nullptr;
  }
  // Carve the chunk: push blocks 1..N-1 to the thread cache, return block 0.
  for (std::size_t i = 1; i < kBatchSize; ++i) {
    char *blk = chunk->base + i * chunk->stride;
    *reinterpret_cast<Chunk **>(blk + 8) = chunk;
    ThreadCachePush(p, idx, blk);
  }
  *reinterpret_cast<Chunk **>(chunk->base + 8) = chunk;
  return chunk->base;
}

// ---- Direct path (over-aligned or oversized) -------------------------------
void *DirectAlloc(SmallObjectAllocatorPartition *p, std::size_t size, std::size_t align) {
  if (align == 0) {
    align = 1;
  }
  const std::size_t slack = align > kHeaderSize ? align : kHeaderSize;
  void *raw = ::operator new(size + kHeaderSize + slack);
  char *base = static_cast<char *>(raw);
  std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(base) + kHeaderSize;
  addr = (addr + align - 1U) & ~(static_cast<std::uintptr_t>(align) - 1U);
  char *returned = reinterpret_cast<char *>(addr);
  auto *hdr = reinterpret_cast<BlockHeader *>(returned - kHeaderSize);
  hdr->magic = kDirectMagic;
  hdr->size_class = 0;
  hdr->chunk_or_raw = raw;
  p->direct_allocs.fetch_add(1, std::memory_order_relaxed);
  return returned;
}

// Bounds the number of decommitted chunks kept per size class: releases the
// oldest (tail) until within the cap.  Called with p->m held.
void TrimDecommitted(SmallObjectAllocatorPartition *p, std::size_t idx,
                     std::size_t chunk_bytes) {
  std::size_t n = 0;
  for (Chunk *c = p->decommitted_heads[idx]; c != nullptr; c = c->decommitted_next) {
    ++n;
  }
  while (n > kMaxDecommittedPerClass) {
    Chunk **tail = &p->decommitted_heads[idx];
    while ((*tail)->decommitted_next != nullptr) {
      tail = &(*tail)->decommitted_next;
    }
    Chunk *oldest = *tail;
    *tail = nullptr;
    PageFree(oldest->base, chunk_bytes);
    p->reserved_bytes.fetch_sub(chunk_bytes, std::memory_order_relaxed);
    p->released_bytes.fetch_add(chunk_bytes, std::memory_order_relaxed);
    delete oldest;
    --n;
  }
}

// Decommits fully-free chunks of a partition: physical pages are returned to
// the OS, the virtual address range is kept in the per-class reuse list.
void PurgePartition(SmallObjectAllocatorPartition *p) {
  // First flush the calling thread's cache so its blocks become reclaimable.
  for (std::size_t sc = 0; sc < kNumSizeClasses; ++sc) {
    FlushAllToCentral(p, sc);
  }

  // Walk the committed chunk list under the partition lock and decommit chunks
  // whose blocks are all idle in the central pool.
  std::lock_guard<std::mutex> lock(p->m);
  Chunk **pp = &p->chunks;
  while (*pp != nullptr) {
    Chunk *chunk = *pp;
    if (chunk->central_count != kBatchSize ||
        chunk->in_use.load(std::memory_order_relaxed) != 0) {
      pp = &chunk->next;
      continue;
    }

    // Every block is free and none sit in a thread cache -> all in central.
    // Remove this chunk's blocks from the central free list.
    void *kept_head = nullptr;
    void *block = p->heads[chunk->size_class];
    while (block != nullptr) {
      void *next = FreelistDecode(p, *reinterpret_cast<std::uintptr_t *>(block));
      if (ChunkOf(block) != chunk) {
        *reinterpret_cast<std::uintptr_t *>(block) = FreelistEncode(p, kept_head);
        kept_head = block;
      }
      block = next;
    }
    p->heads[chunk->size_class] = kept_head;

    // Unlink from the committed list, decommit pages, park for reuse.
    *pp = chunk->next;
    const std::size_t chunk_bytes = static_cast<std::size_t>(chunk->stride) * kBatchSize;
    PageDiscard(chunk->base, chunk_bytes);
    chunk->committed = false;
    chunk->decommitted_next = p->decommitted_heads[chunk->size_class];
    p->decommitted_heads[chunk->size_class] = chunk;
    p->committed_bytes.fetch_sub(chunk_bytes, std::memory_order_relaxed);
    p->chunk_purges.fetch_add(1, std::memory_order_relaxed);
    TrimDecommitted(p, chunk->size_class, chunk_bytes);
    // keep pp unchanged (chunk replaced by its next)
  }
}

} // namespace

// ---- Exported API ----------------------------------------------------------

void *SmallObjectAllocInPartition(SmallObjectAllocatorPartition *partition,
                                  std::size_t size, std::size_t alignment) {
  SmallObjectAllocatorPartition *p =
      partition != nullptr ? partition : &DefaultPartition();
  if (size > kMaxPooledSize || alignment > kMaxAlign) {
    return DirectAlloc(p, size, alignment);
  }
  const std::size_t idx = IndexForSize(size);
  if (idx >= kNumSizeClasses) {
    return DirectAlloc(p, size, alignment);
  }

  ThreadCache &tc = g_tls_cache[p->index];
  // Counter-based self-purge: periodically shed half the current thread cache.
  // Memory-pressure detection intentionally does NOT run here (no syscall on
  // the hot path) — it lives in the independent
  // PurgeSmallObjectAllocatorIfOverWatermark(), called by the application.
  if (++tc.alloc_count % kAllocBatch == 0) {
    for (std::size_t sc = 0; sc < kNumSizeClasses; ++sc) {
      if (tc.counts[sc] > kThreadCacheCap / 2) {
        FlushToCentral(p, sc, tc.counts[sc] / 2);
      }
    }
  }

  void *block = ThreadCachePop(p, idx);
  if (block == nullptr) {
    block = Refill(p, idx);
  }
  if (block == nullptr) {
    throw std::bad_alloc();
  }
  Chunk *chunk = ChunkOf(block);
  chunk->in_use.fetch_add(1, std::memory_order_relaxed);
  p->in_use_by_class[idx].fetch_add(1, std::memory_order_relaxed);
  auto *hdr = static_cast<BlockHeader *>(block);
  hdr->magic = kPooledMagic;
  hdr->size_class = static_cast<std::uint32_t>(idx);
  hdr->chunk_or_raw = chunk;
  p->pooled_allocs.fetch_add(1, std::memory_order_relaxed);
  return static_cast<char *>(block) + kHeaderSize;
}

void *SmallObjectAlloc(std::size_t size, std::size_t alignment) {
  return SmallObjectAllocInPartition(nullptr, size, alignment);
}

void SmallObjectFree(void *ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }
  auto *hdr = reinterpret_cast<BlockHeader *>(static_cast<char *>(ptr) - kHeaderSize);
  if (hdr->magic == kPooledMagic) {
    Chunk *chunk = static_cast<Chunk *>(hdr->chunk_or_raw);
    SmallObjectAllocatorPartition *p = chunk->partition;
    const std::size_t idx = hdr->size_class;
    if (idx < kNumSizeClasses) {
      chunk->in_use.fetch_sub(1, std::memory_order_relaxed);
      p->in_use_by_class[idx].fetch_sub(1, std::memory_order_relaxed);
      ThreadCache &tc = g_tls_cache[p->index];
      if (tc.counts[idx] >= kThreadCacheCap) {
        FlushToCentral(p, idx, tc.counts[idx] / 2);
      }
      ThreadCachePush(p, idx, hdr);
      p->pooled_frees.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (hdr->magic == kDirectMagic) {
    ::operator delete(hdr->chunk_or_raw);
  }
  // Unknown magic: ignore (should not happen in correct use).
}

void PurgeSmallObjectAllocatorPartition(SmallObjectAllocatorPartition *partition) {
  if (partition == nullptr) {
    return;
  }
  PurgePartition(partition);
}

void PurgeSmallObjectAllocator() {
  PurgePartition(&DefaultPartition());
}

SmallObjectAllocatorPartition *CreateSmallObjectAllocatorPartition() {
  const std::uint32_t index = g_num_created.fetch_add(1, std::memory_order_relaxed);
  if (index >= kMaxPartitions) {
    g_num_created.fetch_sub(1, std::memory_order_relaxed);
    return nullptr;
  }
  auto *p = new SmallObjectAllocatorPartition;
  p->index = index;
  p->freelist_key = GenerateFreelistKey();
  g_partitions[index].store(p, std::memory_order_relaxed);
  return p;
}

void DestroySmallObjectAllocatorPartition(SmallObjectAllocatorPartition *partition) {
  if (partition == nullptr) {
    return;
  }
  // Release every remaining chunk (the caller must have freed all outstanding
  // blocks).  This never touches blocks in thread caches of other threads.
  std::lock_guard<std::mutex> lock(partition->m);
  Chunk *c = partition->chunks;
  while (c != nullptr) {
    Chunk *next = c->next;
    PageFree(c->base, static_cast<std::size_t>(c->stride) * kBatchSize);
    delete c;
    c = next;
  }
  partition->chunks = nullptr;
  for (std::size_t sc = 0; sc < kNumSizeClasses; ++sc) {
    Chunk *d = partition->decommitted_heads[sc];
    while (d != nullptr) {
      Chunk *next = d->decommitted_next;
      PageFree(d->base, static_cast<std::size_t>(d->stride) * kBatchSize);
      delete d;
      d = next;
    }
    partition->decommitted_heads[sc] = nullptr;
  }
  g_partitions[partition->index].store(nullptr, std::memory_order_relaxed);
  delete partition;
}

void GetSmallObjectAllocatorPartitionStats(SmallObjectAllocatorPartition *partition,
                                           SmallObjectAllocatorStats *out) {
  if (out == nullptr) {
    return;
  }
  SmallObjectAllocatorPartition *p =
      partition != nullptr ? partition : &DefaultPartition();
  out->pooled_allocs = p->pooled_allocs.load(std::memory_order_relaxed);
  out->pooled_frees = p->pooled_frees.load(std::memory_order_relaxed);
  out->direct_allocs = p->direct_allocs.load(std::memory_order_relaxed);
  out->chunk_allocs = p->chunk_allocs.load(std::memory_order_relaxed);
  out->chunk_purges = p->chunk_purges.load(std::memory_order_relaxed);
  out->reserved_bytes = p->reserved_bytes.load(std::memory_order_relaxed);
  out->committed_bytes = p->committed_bytes.load(std::memory_order_relaxed);
  out->released_bytes = p->released_bytes.load(std::memory_order_relaxed);
}

void GetSmallObjectAllocatorStats(SmallObjectAllocatorStats *out) {
  GetSmallObjectAllocatorPartitionStats(nullptr, out);
}

void ResetSmallObjectAllocatorStats() {
  SmallObjectAllocatorPartition &p = DefaultPartition();
  p.pooled_allocs.store(0, std::memory_order_relaxed);
  p.pooled_frees.store(0, std::memory_order_relaxed);
  p.direct_allocs.store(0, std::memory_order_relaxed);
  p.chunk_allocs.store(0, std::memory_order_relaxed);
  p.chunk_purges.store(0, std::memory_order_relaxed);
  p.reserved_bytes.store(0, std::memory_order_relaxed);
  p.committed_bytes.store(0, std::memory_order_relaxed);
  p.released_bytes.store(0, std::memory_order_relaxed);
  // Note: in_use_by_class holds live counts and is intentionally not reset.
}

std::size_t GetSmallObjectAllocatorPartitionSizeClassStats(
    SmallObjectAllocatorPartition *partition, SmallObjectAllocatorSizeClassStats *out,
    std::size_t capacity) {
  if (out == nullptr || capacity == 0) {
    return 0;
  }
  SmallObjectAllocatorPartition *p =
      partition != nullptr ? partition : &DefaultPartition();
  std::lock_guard<std::mutex> lock(p->m);
  std::size_t n = 0;
  for (std::size_t i = 0; i < kNumSizeClasses && n < capacity; ++i) {
    std::uint64_t central_free = 0;
    for (void *b = p->heads[i]; b != nullptr;
         b = FreelistDecode(p, *reinterpret_cast<std::uintptr_t *>(b))) {
      ++central_free;
    }
    out[n].size = kUsableSizes[i];
    out[n].in_use = p->in_use_by_class[i].load(std::memory_order_relaxed);
    out[n].central_free = central_free;
    ++n;
  }
  return n;
}

std::size_t GetSmallObjectAllocatorSizeClassStats(SmallObjectAllocatorSizeClassStats *out,
                                                  std::size_t capacity) {
  return GetSmallObjectAllocatorPartitionSizeClassStats(nullptr, out, capacity);
}

} // namespace nei
