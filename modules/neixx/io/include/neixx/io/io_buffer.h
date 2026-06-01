#pragma once

#ifndef NEIXX_IO_IO_BUFFER_H_
#define NEIXX_IO_IO_BUFFER_H_

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

class NEI_API IOBuffer : public RefCountedThreadSafe<IOBuffer> {
 public:
  char* data() { return data_; }
  const char* data() const { return data_; }

  IOBuffer(const IOBuffer&) = delete;
  IOBuffer& operator=(const IOBuffer&) = delete;

 protected:
  explicit IOBuffer(char* data);
  virtual ~IOBuffer();

  void set_data(char* data) { data_ = data; }

 private:
  friend class RefCountedThreadSafe<IOBuffer>;

  char* data_ = nullptr;
};

class IOBufferPool;

class NEI_API IOBufferWithSize : public IOBuffer {
 public:
  explicit IOBufferWithSize(std::size_t size);

  std::size_t size() const { return size_; }

  IOBufferWithSize(const IOBufferWithSize&) = delete;
  IOBufferWithSize& operator=(const IOBufferWithSize&) = delete;

 protected:
  using RecycleFunc = void (*)(void* context,
                               std::size_t block_size,
                               std::unique_ptr<char[]> storage);

  IOBufferWithSize(std::size_t size,
                   std::unique_ptr<char[]> storage,
                   RecycleFunc recycle_func,
                   void* recycle_context);
  ~IOBufferWithSize() override;

 private:
  friend class IOBufferPool;

  std::unique_ptr<char[]> storage_;
  std::size_t size_ = 0;
  RecycleFunc recycle_func_ = nullptr;
  void* recycle_context_ = nullptr;
};

class NEI_API WrappedIOBuffer final : public IOBuffer {
 public:
  explicit WrappedIOBuffer(char* data);

  WrappedIOBuffer(const WrappedIOBuffer&) = delete;
  WrappedIOBuffer& operator=(const WrappedIOBuffer&) = delete;

 protected:
  ~WrappedIOBuffer() override;
};

class NEI_API DrainableIOBuffer final : public IOBuffer {
 public:
  DrainableIOBuffer(scoped_refptr<IOBuffer> base_buffer, std::size_t size);

  void DidConsume(std::size_t bytes);
  std::size_t BytesRemaining() const;
  std::size_t BytesConsumed() const;

  scoped_refptr<IOBuffer> base_buffer() const { return base_buffer_; }

  DrainableIOBuffer(const DrainableIOBuffer&) = delete;
  DrainableIOBuffer& operator=(const DrainableIOBuffer&) = delete;

 protected:
  ~DrainableIOBuffer() override;

 private:
  void RefreshDataPointer();

  scoped_refptr<IOBuffer> base_buffer_;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

class NEI_API IOBufferPool {
 public:
  static IOBufferPool& GetInstance();

  // Acquires a reusable buffer. For hot bucket sizes (4KB and 64KB), this
  // avoids repetitive heap churn by reusing cached blocks.
  scoped_refptr<IOBufferWithSize> AcquireBuffer(std::size_t size);

  // Test hooks for deterministic pool behavior verification.
  void SetBucketLimitForTesting(std::size_t bucket_size,
                                std::size_t max_cached_blocks);
  void ClearForTesting();

  IOBufferPool(const IOBufferPool&) = delete;
  IOBufferPool& operator=(const IOBufferPool&) = delete;

 private:
  struct Bucket {
    std::size_t block_size = 0;
    std::size_t max_cached_blocks = 0;
    std::vector<std::unique_ptr<char[]>> free_blocks;
  };

  IOBufferPool();
  ~IOBufferPool();

  static void RecycleStorageThunk(void* context,
                                  std::size_t block_size,
                                  std::unique_ptr<char[]> storage);
  void RecycleStorage(std::size_t block_size, std::unique_ptr<char[]> storage);

  std::size_t NormalizeBucketSize(std::size_t requested_size) const;
  Bucket& GetOrCreateBucket(std::size_t block_size);

  mutable std::mutex lock_;
  std::vector<Bucket> buckets_;
};

}  // namespace nei

#endif  // NEIXX_IO_IO_BUFFER_H_
