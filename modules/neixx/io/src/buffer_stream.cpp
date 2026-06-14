// ===========================================================================
// BufferInputStream implementation
// ===========================================================================

#include <neixx/io/buffer_stream.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <nei/debug/check.h>

namespace nei {

// ---- Owning constructors --------------------------------------------------

BufferInputStream::BufferInputStream(std::vector<std::uint8_t> data)
    : owned_data_(std::move(data)),
      data_(owned_data_.data()),
      len_(owned_data_.size()) {
}

BufferInputStream::BufferInputStream(std::string data)
    : owned_data_(data.begin(), data.end()),
      data_(owned_data_.data()),
      len_(owned_data_.size()) {
}

// ---- Borrowing constructors -----------------------------------------------

BufferInputStream::BufferInputStream(const std::uint8_t* data,
                                     std::size_t len)
    : data_(data), len_(len) {
  DCHECK(data_ != nullptr || len == 0);
}

BufferInputStream::BufferInputStream(const char* data, std::size_t len)
    : BufferInputStream(reinterpret_cast<const std::uint8_t*>(data), len) {
}

BufferInputStream::BufferInputStream(scoped_refptr<IOBuffer> buf,
                                     std::size_t len)
    : owned_buf_(std::move(buf)),
      data_(reinterpret_cast<const std::uint8_t*>(owned_buf_->data())),
      len_(len) {
  DCHECK(data_ != nullptr || len == 0);
}

BufferInputStream::~BufferInputStream() {
  Close();
}

// ---- AsyncInputStream -----------------------------------------------------

void BufferInputStream::ReadAsync(scoped_refptr<IOBuffer> buf,
                                  std::size_t buf_len,
                                  IOReadCallback callback) {
  if (closed_) {
    if (callback) callback(false, 0);
    return;
  }

  const std::size_t avail = remaining();
  if (avail == 0) {
    // EOF — data already exhausted.
    if (callback) callback(false, 0);
    return;
  }

  const std::size_t to_copy = std::min(avail, buf_len);
  std::memcpy(buf->data(), data_ + cursor_, to_copy);
  cursor_ += to_copy;

  if (callback) callback(true, to_copy);
}

void BufferInputStream::Close() {
  closed_ = true;
  owned_data_.clear();
  owned_buf_.reset();
  data_ = nullptr;
  len_ = 0;
  cursor_ = 0;
}

std::size_t BufferInputStream::remaining() const {
  return (cursor_ < len_) ? (len_ - cursor_) : 0;
}

}  // namespace nei
