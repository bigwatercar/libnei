#include <neixx/io/io_buffer.h>

#include <algorithm>
#include <utility>

#include <nei/debug/check.h>

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

}  // namespace

IOBuffer::IOBuffer(char* data) : data_(data) {}

IOBuffer::~IOBuffer() = default;

IOBufferWithSize::IOBufferWithSize(std::size_t size)
    : IOBufferWithSize(size,
                       std::make_unique<char[]>(std::max<std::size_t>(1u, size)),
                       nullptr,
                       nullptr) {}

IOBufferWithSize::IOBufferWithSize(std::size_t size,
                                   std::unique_ptr<char[]> storage,
                                   RecycleFunc recycle_func,
                                   void* recycle_context)
    : IOBuffer(storage.get()),
      storage_(std::move(storage)),
      size_(size),
      recycle_func_(recycle_func),
      recycle_context_(recycle_context) {
  DCHECK(storage_ != nullptr);
}

IOBufferWithSize::~IOBufferWithSize() {
  if (recycle_func_ != nullptr && storage_ != nullptr) {
    recycle_func_(recycle_context_, size_, std::move(storage_));
  }
}

WrappedIOBuffer::WrappedIOBuffer(char* data) : IOBuffer(data) {}

WrappedIOBuffer::~WrappedIOBuffer() = default;

DrainableIOBuffer::DrainableIOBuffer(scoped_refptr<IOBuffer> base_buffer,
                                     std::size_t size)
    : IOBuffer(nullptr), base_buffer_(std::move(base_buffer)), size_(size) {
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

IOBufferPool& IOBufferPool::GetInstance() {
  static IOBufferPool pool;
  return pool;
}

scoped_refptr<IOBufferWithSize> IOBufferPool::AcquireBuffer(std::size_t size) {
  DCHECK_GT(size, 0u);
  const std::size_t normalized_size = NormalizeBucketSize(std::max<std::size_t>(1u, size));

  std::unique_ptr<char[]> storage;
  if (IsPooledBucket(normalized_size)) {
    std::lock_guard<std::mutex> lock(lock_);
    Bucket& bucket = GetOrCreateBucket(normalized_size);
    if (!bucket.free_blocks.empty()) {
      storage = std::move(bucket.free_blocks.back());
      bucket.free_blocks.pop_back();
    }
  }

  if (storage == nullptr) {
    storage = std::make_unique<char[]>(normalized_size);
  }

  IOBufferWithSize::RecycleFunc recycle_func = nullptr;
  void* recycle_context = nullptr;
  if (IsPooledBucket(normalized_size)) {
    recycle_func = &IOBufferPool::RecycleStorageThunk;
    recycle_context = this;
  }

  return scoped_refptr<IOBufferWithSize>(
      new IOBufferWithSize(normalized_size,
                           std::move(storage),
                           recycle_func,
                           recycle_context));
}

void IOBufferPool::SetBucketLimitForTesting(std::size_t bucket_size,
                                            std::size_t max_cached_blocks) {
  std::lock_guard<std::mutex> lock(lock_);
  Bucket& bucket = GetOrCreateBucket(bucket_size);
  bucket.max_cached_blocks = max_cached_blocks;
  while (bucket.free_blocks.size() > bucket.max_cached_blocks) {
    bucket.free_blocks.pop_back();
  }
}

void IOBufferPool::ClearForTesting() {
  std::lock_guard<std::mutex> lock(lock_);
  for (Bucket& bucket : buckets_) {
    bucket.free_blocks.clear();
  }
}

IOBufferPool::IOBufferPool() = default;

IOBufferPool::~IOBufferPool() = default;

// static
void IOBufferPool::RecycleStorageThunk(void* context,
                                       std::size_t block_size,
                                       std::unique_ptr<char[]> storage) {
  if (context == nullptr || storage == nullptr) {
    return;
  }

  static_cast<IOBufferPool*>(context)->RecycleStorage(block_size, std::move(storage));
}

void IOBufferPool::RecycleStorage(std::size_t block_size,
                                  std::unique_ptr<char[]> storage) {
  DCHECK(storage != nullptr);
  if (!IsPooledBucket(block_size) || storage == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(lock_);
  Bucket& bucket = GetOrCreateBucket(block_size);
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

IOBufferPool::Bucket& IOBufferPool::GetOrCreateBucket(std::size_t block_size) {
  for (Bucket& bucket : buckets_) {
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

}  // namespace nei
