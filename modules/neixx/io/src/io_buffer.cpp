#include <neixx/io/io_buffer.h>

#include <cstring>
#include <cstdlib>

#include "io_buffer_platform.h"

namespace nei {

namespace {

std::size_t NormalizeAlignment(std::size_t alignment) {
  std::size_t out = alignment;
  if (out < sizeof(void *)) {
    out = sizeof(void *);
  }

  if ((out & (out - 1U)) != 0U) {
    std::size_t p = 1U;
    while (p < out) {
      p <<= 1U;
    }
    out = p;
  }
  return out;
}

uint8_t *AllocatePlain(std::size_t size) {
  if (size == 0U) {
    return nullptr;
  }
  void *ptr = std::malloc(size);
  return static_cast<uint8_t *>(ptr);
}

} // namespace

IOBuffer::IOBuffer(std::size_t size)
    : data_(AllocatePlain(size)), size_(size), alignment_(alignof(std::max_align_t)), aligned_alloc_(false) {
  if (data_ != nullptr && size_ > 0U) {
    std::memset(data_, 0, size_);
  }
}

IOBuffer::IOBuffer(std::size_t size, std::size_t alignment)
    : data_(detail::AllocateAlignedPlatform(size, NormalizeAlignment(alignment))),
      size_(size),
      alignment_(NormalizeAlignment(alignment)),
      aligned_alloc_(true) {
  if (data_ != nullptr && size_ > 0U) {
    std::memset(data_, 0, size_);
  }
}

scoped_refptr<IOBuffer> IOBuffer::CreatePageAligned(std::size_t size, std::size_t alignment) {
  if (alignment == 0U) {
    alignment = 4096U;
  }
  return MakeRefCounted<IOBuffer>(size, NormalizeAlignment(alignment));
}

IOBuffer::~IOBuffer() {
  detail::FreeBufferPlatform(data_, aligned_alloc_);
  data_ = nullptr;
  size_ = 0;
}

uint8_t *IOBuffer::data() noexcept {
  return data_;
}

const uint8_t *IOBuffer::data() const noexcept {
  return data_;
}

std::size_t IOBuffer::size() const noexcept {
  return size_;
}

std::size_t IOBuffer::alignment() const noexcept {
  return alignment_;
}

bool IOBuffer::is_page_aligned() const noexcept {
  return aligned_alloc_;
}

} // namespace nei
