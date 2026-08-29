// ===========================================================================
// BufferInputStream implementation (PIMPL)
// ===========================================================================

#include <neixx/io/buffer_stream.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <nei/debug/check.h>

namespace nei {

struct BufferInputStream::Impl {
  // Owned storage (populated by the owning constructors).
  std::vector<std::uint8_t> owned_data;
  scoped_refptr<IOBuffer> owned_buf;

  // View of the active data source.
  const std::uint8_t *data = nullptr;
  std::size_t len = 0;
  std::size_t cursor = 0;

  bool closed = false;
};

// ---- Owning constructors --------------------------------------------------

BufferInputStream::BufferInputStream(std::vector<std::uint8_t> data)
    : impl_(std::make_unique<Impl>()) {
  impl_->owned_data = std::move(data);
  impl_->data = impl_->owned_data.data();
  impl_->len = impl_->owned_data.size();
}

BufferInputStream::BufferInputStream(std::string data)
    : impl_(std::make_unique<Impl>()) {
  impl_->owned_data.assign(data.begin(), data.end());
  impl_->data = impl_->owned_data.data();
  impl_->len = impl_->owned_data.size();
}

// ---- Borrowing constructors -----------------------------------------------

BufferInputStream::BufferInputStream(const std::uint8_t *data, std::size_t len)
    : impl_(std::make_unique<Impl>()) {
  DCHECK(data != nullptr || len == 0);
  impl_->data = data;
  impl_->len = len;
}

BufferInputStream::BufferInputStream(const char *data, std::size_t len)
    : BufferInputStream(reinterpret_cast<const std::uint8_t *>(data), len) {
}

BufferInputStream::BufferInputStream(scoped_refptr<IOBuffer> buf, std::size_t len)
    : impl_(std::make_unique<Impl>()) {
  impl_->owned_buf = std::move(buf);
  impl_->data = reinterpret_cast<const std::uint8_t *>(impl_->owned_buf->data());
  impl_->len = len;
  DCHECK(impl_->data != nullptr || len == 0);
}

BufferInputStream::~BufferInputStream() {
  Close();
}

// ---- AsyncInputStream -----------------------------------------------------

void BufferInputStream::ReadAsync(scoped_refptr<IOBuffer> buf, std::size_t buf_len, IOReadCallback callback) {
  if (impl_->closed) {
    if (callback)
      callback(false, 0);
    return;
  }

  const std::size_t avail = remaining();
  if (avail == 0) {
    if (callback)
      callback(false, 0);
    return;
  }

  const std::size_t to_copy = std::min(avail, buf_len);
  std::memcpy(buf->data(), impl_->data + impl_->cursor, to_copy);
  impl_->cursor += to_copy;

  if (callback)
    callback(true, to_copy);
}

void BufferInputStream::Close() {
  impl_->closed = true;
  impl_->owned_data.clear();
  impl_->owned_buf.reset();
  impl_->data = nullptr;
  impl_->len = 0;
  impl_->cursor = 0;
}

std::size_t BufferInputStream::size() const {
  return impl_->len;
}

std::size_t BufferInputStream::remaining() const {
  return (impl_->cursor < impl_->len) ? (impl_->len - impl_->cursor) : 0;
}

} // namespace nei
