#include <neixx/io/io_buffer.h>

#include <algorithm>
#include <mutex>
#include <utility>

#include <nei/debug/check.h>

// ---------------------------------------------------------------------------
// LeakySingletonTraits specialization for IOBufferPool
// ---------------------------------------------------------------------------
//
// Overrides the default Leaky behavior (pure no-op Delete) to call
// PurgeMemory(), which drains all cached 4K/64K blocks but keeps
// the IOBufferPool shell alive.  This is the Chromium "Leaky LazyInstance"
// pattern: release internal resources, never delete the singleton itself.
//
// Defined here (not in the header) because it depends on the full definition
// of IOBufferPool, which is only available in this translation unit.
template <>
void nei::LeakySingletonTraits<nei::IOBufferPool>::Delete(nei::IOBufferPool *x) {
  if (x) {
    x->PurgeMemory();
    // Intentionally do NOT delete x  --  the shell stays alive to prevent
    // use-after-free crashes from background threads during shutdown.
  }
}

namespace nei {

namespace {

constexpr std::size_t kPage4K = 4u * 1024u;
constexpr std::size_t kPage64K = 64u * 1024u;
constexpr std::size_t kDefault4KCacheLimit = 256u;
constexpr std::size_t kDefault64KCacheLimit = 64u;

bool IsPooledBucket(std::size_t block_size) {
  return block_size == kPage4K || block_size == kPage64K;
}

std::size_t DefaultBucketLimit(std::size_t block_size) {
  switch (block_size) {
  case kPage4K:
    return kDefault4KCacheLimit;
  case kPage64K:
    return kDefault64KCacheLimit;
  default:
    return 0u;
  }
}

} // namespace

IOBuffer::IOBuffer(unsigned char *data)
    : data_(data) {
}

IOBuffer::~IOBuffer() = default;

IOBufferWithSize::IOBufferWithSize(std::size_t size)
    : IOBuffer(nullptr)
    , storage_(std::make_unique<unsigned char[]>(std::max<std::size_t>(1u, size)))
    , size_(size) {
  set_data(storage_.get());
}

IOBufferWithSize::~IOBufferWithSize() = default;

PooledIOBuffer::PooledIOBuffer(std::size_t capacity,
                               std::unique_ptr<unsigned char[]> storage,
                               RecycleFunc recycle_func,
                               void *recycle_context)
    : IOBuffer(storage.get())
    , storage_(std::move(storage))
    , capacity_(capacity)
    , recycle_func_(recycle_func)
    , recycle_context_(recycle_context) {
  DCHECK(storage_ != nullptr);
}

PooledIOBuffer::~PooledIOBuffer() {
  if (recycle_func_ != nullptr && storage_ != nullptr) {
    recycle_func_(recycle_context_, capacity_, std::move(storage_));
  }
}

WrappedIOBuffer::WrappedIOBuffer(unsigned char *data)
    : IOBuffer(data) {
}

WrappedIOBuffer::~WrappedIOBuffer() = default;

DrainableIOBuffer::DrainableIOBuffer(scoped_refptr<IOBuffer> base_buffer, std::size_t size)
    : IOBuffer(nullptr)
    , base_buffer_(std::move(base_buffer))
    , size_(size) {
  DCHECK(base_buffer_ != nullptr);
  RefreshDataPointer();
}

DrainableIOBuffer::~DrainableIOBuffer() = default;

void DrainableIOBuffer::DidConsume(std::size_t bytes) {
  // This invariant protects virtual slicing from integer wrap-around and from
  // advancing beyond the originally declared window. DCHECK keeps debug builds
  // strict while the release fallback prevents UB in production.
  DCHECK_LE(bytes, BytesRemaining());
  if (bytes > BytesRemaining()) {
    offset_ = size_;
    RefreshDataPointer();
    return;
  }

  offset_ += bytes;
  RefreshDataPointer();
}

std::size_t DrainableIOBuffer::BytesRemaining() const {
  return size_ - offset_;
}

std::size_t DrainableIOBuffer::BytesConsumed() const {
  return offset_;
}

void DrainableIOBuffer::RefreshDataPointer() {
  if (base_buffer_.get() == nullptr || base_buffer_->data() == nullptr) {
    set_data(nullptr);
    return;
  }

  // Pointer arithmetic here is intentionally derived from validated virtual
  // offsets only. The object never mutates base_buffer_ memory ownership.
  set_data(base_buffer_->data() + offset_);
}

IOBufferPool &IOBufferPool::GetInstance() {
  // Delegate to the Singleton template with LeakySingletonTraits.
  //
  // The specialized LeakySingletonTraits<IOBufferPool>::Delete() (defined
  // above) calls PurgeMemory() to drain cached 4K/64K blocks at exit,
  // while intentionally leaking the pool shell itself.
  //
  // The Singleton template handles:
  //   - Double-Checked Locking with std::atomic acquire-release barriers
  //   - AtExitManager callback registration
  //   - CHECK_MSG if AtExitManager is missing
  return *Singleton<IOBufferPool, LeakySingletonTraits<IOBufferPool>>::GetInstance();
}

scoped_refptr<PooledIOBuffer> IOBufferPool::AcquireBuffer(std::size_t size) {
  DCHECK_GT(size, 0u);
  const std::size_t normalized_size = NormalizeBucketSize(std::max<std::size_t>(1u, size));

  std::unique_ptr<unsigned char[]> storage;
  if (IsPooledBucket(normalized_size)) {
    std::lock_guard<std::mutex> lock(lock_);
    Bucket &bucket = GetOrCreateBucket(normalized_size);
    if (!bucket.free_blocks.empty()) {
      storage = std::move(bucket.free_blocks.back());
      bucket.free_blocks.pop_back();
    }
  }

  if (storage == nullptr) {
    storage = std::make_unique<unsigned char[]>(normalized_size);
  }

  PooledIOBuffer::RecycleFunc recycle_func = nullptr;
  void *recycle_context = nullptr;
  if (IsPooledBucket(normalized_size)) {
    recycle_func = &IOBufferPool::RecycleStorageThunk;
    recycle_context = this;
  }

  return scoped_refptr<PooledIOBuffer>(
      new PooledIOBuffer(normalized_size, std::move(storage), recycle_func, recycle_context));
}

void IOBufferPool::SetBucketLimitForTesting(std::size_t bucket_size, std::size_t max_cached_blocks) {
  std::lock_guard<std::mutex> lock(lock_);
  Bucket &bucket = GetOrCreateBucket(bucket_size);
  bucket.max_cached_blocks = max_cached_blocks;
  while (bucket.free_blocks.size() > bucket.max_cached_blocks) {
    bucket.free_blocks.pop_back();
  }
}

void IOBufferPool::PurgeMemory() {
  std::lock_guard<std::mutex> lock(lock_);
  for (Bucket &bucket : buckets_) {
    bucket.free_blocks.clear();
  }
}

IOBufferPool::IOBufferPool() = default;

IOBufferPool::~IOBufferPool() = default;

// static
void IOBufferPool::RecycleStorageThunk(void *context,
                                       std::size_t block_size,
                                       std::unique_ptr<unsigned char[]> storage) {
  if (context == nullptr || storage == nullptr) {
    return;
  }

  static_cast<IOBufferPool *>(context)->RecycleStorage(block_size, std::move(storage));
}

void IOBufferPool::RecycleStorage(std::size_t block_size, std::unique_ptr<unsigned char[]> storage) {
  DCHECK(storage != nullptr);
  if (!IsPooledBucket(block_size) || storage == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(lock_);
  Bucket &bucket = GetOrCreateBucket(block_size);
  if (bucket.free_blocks.size() >= bucket.max_cached_blocks) {
    return;
  }
  bucket.free_blocks.push_back(std::move(storage));
}

std::size_t IOBufferPool::NormalizeBucketSize(std::size_t requested_size) const {
  if (requested_size <= kPage4K) {
    return kPage4K;
  }
  if (requested_size <= kPage64K) {
    return kPage64K;
  }
  return requested_size;
}

IOBufferPool::Bucket &IOBufferPool::GetOrCreateBucket(std::size_t block_size) {
  for (Bucket &bucket : buckets_) {
    if (bucket.block_size == block_size) {
      return bucket;
    }
  }

  Bucket new_bucket;
  new_bucket.block_size = block_size;
  new_bucket.max_cached_blocks = DefaultBucketLimit(block_size);
  buckets_.push_back(std::move(new_bucket));
  return buckets_.back();
}

} // namespace nei
