#pragma once

#ifndef NEIXX_IO_IO_BUFFER_H_
#define NEIXX_IO_IO_BUFFER_H_

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include <nei/build/compiler_specific.h>
#include <nei/build/nei_export.h>
#include <neixx/common/singleton.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

// Base reference-counted byte buffer view.
//
// IOBuffer models pointer semantics (`unsigned char* data()`).  The return
// type is unsigned char to prevent MSVC sign-extension bugs when individual
// bytes are promoted to wider integer types (e.g. XOR with uint64_t).
// Ownership of the underlying memory is defined by subclasses.
class NEI_API IOBuffer : public RefCountedThreadSafe<IOBuffer> {
public:
  // Returns mutable storage start. May be null for empty/unbound wrappers.
  unsigned char *data() {
    return data_;
  }

  // Returns immutable storage start. May be null for empty/unbound wrappers.
  const unsigned char *data() const {
    return data_;
  }

  IOBuffer(const IOBuffer &) = delete;
  IOBuffer &operator=(const IOBuffer &) = delete;

protected:
  explicit IOBuffer(unsigned char *data);
  virtual ~IOBuffer();

  void set_data(unsigned char *data) {
    data_ = data;
  }

private:
  friend class RefCountedThreadSafe<IOBuffer>;

  unsigned char *data_ = nullptr;
};

class IOBufferPool;

// Owns a contiguous heap allocation with explicit byte size metadata.
//
// This is the primary concrete buffer for read/write operations. Instances can
// optionally return their storage to IOBufferPool during destruction.
class NEI_API IOBufferWithSize : public IOBuffer {
public:
  // Allocates a buffer with `size` bytes.
  explicit IOBufferWithSize(std::size_t size);

  // Number of valid bytes in this allocation.
  std::size_t size() const {
    return size_;
  }

  IOBufferWithSize(const IOBufferWithSize &) = delete;
  IOBufferWithSize &operator=(const IOBufferWithSize &) = delete;

protected:
  using RecycleFunc = void (*)(void *context, std::size_t block_size, std::unique_ptr<unsigned char[]> storage);

  IOBufferWithSize(std::size_t size,
                   std::unique_ptr<unsigned char[]> storage,
                   RecycleFunc recycle_func,
                   void *recycle_context);
  ~IOBufferWithSize() override;

private:
  friend class IOBufferPool;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::unique_ptr<unsigned char[]> storage_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
  std::size_t size_ = 0;
  RecycleFunc recycle_func_ = nullptr;
  void *recycle_context_ = nullptr;
};

class NEI_API WrappedIOBuffer final : public IOBuffer {
public:
  // Wraps externally owned storage without taking ownership.
  //
  // The wrapped pointer must outlive this wrapper. Destruction never frees
  // the wrapped memory.
  explicit WrappedIOBuffer(unsigned char *data);

  WrappedIOBuffer(const WrappedIOBuffer &) = delete;
  WrappedIOBuffer &operator=(const WrappedIOBuffer &) = delete;

protected:
  ~WrappedIOBuffer() override;
};

class NEI_API DrainableIOBuffer final : public IOBuffer {
public:
  // Creates a virtual slice on top of `base_buffer` with a logical size
  // window `[0, size)`. The visible data pointer moves as bytes are consumed.
  DrainableIOBuffer(scoped_refptr<IOBuffer> base_buffer, std::size_t size);

  // Advances the logical read/write cursor by `bytes`.
  //
  // In debug builds, over-consume is DCHECKed. In release builds, the cursor
  // is clamped to the end to avoid undefined pointer movement.
  void DidConsume(std::size_t bytes);
  // Remaining bytes in the current virtual window.
  std::size_t BytesRemaining() const;
  // Total bytes consumed since construction.
  std::size_t BytesConsumed() const;

  // Returns the underlying base buffer retained by this slice.
  scoped_refptr<IOBuffer> base_buffer() const {
    return base_buffer_;
  }

  DrainableIOBuffer(const DrainableIOBuffer &) = delete;
  DrainableIOBuffer &operator=(const DrainableIOBuffer &) = delete;

protected:
  ~DrainableIOBuffer() override;

private:
  void RefreshDataPointer();

  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  scoped_refptr<IOBuffer> base_buffer_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

class NEI_API IOBufferPool {
public:
  // Global process-local pool instance.
  static IOBufferPool &GetInstance();

  // Acquires a reusable buffer. For hot bucket sizes (4KB and 64KB), this
  // avoids repetitive heap churn by reusing cached blocks.
  //
  // Returned buffer size may be normalized to a hot bucket size.
  scoped_refptr<IOBufferWithSize> AcquireBuffer(std::size_t size);

  // Test hooks for deterministic pool behavior verification.
  void SetBucketLimitForTesting(std::size_t bucket_size, std::size_t max_cached_blocks);
  void PurgeMemory();

  IOBufferPool(const IOBufferPool &) = delete;
  IOBufferPool &operator=(const IOBufferPool &) = delete;

private:
  struct Bucket {
    std::size_t block_size = 0;
    std::size_t max_cached_blocks = 0;
    std::vector<std::unique_ptr<unsigned char[]>> free_blocks;
  };

  IOBufferPool();
  ~IOBufferPool();

  // Grant the LeakySingletonTraits specialization access to the private
  // constructor so it can perform `new IOBufferPool()` and, at exit,
  // call PurgeMemory() to drain cached blocks without deleting the shell.
  friend struct LeakySingletonTraits<IOBufferPool>;

  static void RecycleStorageThunk(void *context, std::size_t block_size, std::unique_ptr<unsigned char[]> storage);
  void RecycleStorage(std::size_t block_size, std::unique_ptr<unsigned char[]> storage);

  std::size_t NormalizeBucketSize(std::size_t requested_size) const;
  Bucket &GetOrCreateBucket(std::size_t block_size);

  mutable std::mutex lock_;
  NEI_SUPPRESS_MSC_WARNING_4251_BEGIN
  std::vector<Bucket> buckets_;
  NEI_SUPPRESS_MSC_WARNING_4251_END
};

} // namespace nei

#endif // NEIXX_IO_IO_BUFFER_H_
