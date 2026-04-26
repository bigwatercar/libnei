#ifndef NEIXX_IO_IO_BUFFER_H_
#define NEIXX_IO_IO_BUFFER_H_

#include <cstddef>
#include <cstdint>

#include <nei/macros/nei_export.h>
#include <neixx/memory/ref_counted.h>

namespace nei {

class NEI_API IOBuffer final : public RefCountedThreadSafe<IOBuffer> {
public:
  explicit IOBuffer(std::size_t size);
  IOBuffer(std::size_t size, std::size_t alignment);

  static scoped_refptr<IOBuffer> CreatePageAligned(std::size_t size,
                                                   std::size_t alignment = 4096);

  uint8_t *data() noexcept;
  const uint8_t *data() const noexcept;
  std::size_t size() const noexcept;
  std::size_t alignment() const noexcept;
  bool is_page_aligned() const noexcept;

private:
  friend class RefCountedThreadSafe<IOBuffer>;
  ~IOBuffer();

  uint8_t *data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t alignment_ = 0;
  bool aligned_alloc_ = false;
};

} // namespace nei

#endif // NEIXX_IO_IO_BUFFER_H_
