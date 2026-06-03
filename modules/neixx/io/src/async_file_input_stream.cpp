#include <neixx/io/async_file_input_stream.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace nei {

// ---------------------------------------------------------------------------
// AsyncFileInputStreamState
// ---------------------------------------------------------------------------
//
// Pull model: each ReadAsync() call issues exactly one AsyncFile::AsyncRead()
// and fires exactly one callback.  The caller is responsible for issuing the
// next ReadAsync() if it wants more data.  This matches the AsyncInputStream
// pull contract agreed by all concrete implementations in this refactor.
struct AsyncFileInputStreamState {
  explicit AsyncFileInputStreamState(std::unique_ptr<AsyncFile> file_in,
                                     std::size_t chunk_size_in,
                                     std::int64_t start_offset)
      : file(std::move(file_in)),
        chunk_size((std::max)(chunk_size_in, static_cast<std::size_t>(1))),
        offset(start_offset) {}

  std::mutex lock;
  std::unique_ptr<AsyncFile> file;
  std::size_t chunk_size = 4096;
  std::int64_t offset = 0;
  bool closed = false;
  bool read_in_flight = false;  // Prevents overlapping reads.
};

// ---------------------------------------------------------------------------
// AsyncFileInputStream
// ---------------------------------------------------------------------------

AsyncFileInputStream::AsyncFileInputStream(std::unique_ptr<AsyncFile> file,
                                           std::size_t chunk_size,
                                           std::int64_t start_offset)
    : state_(std::make_shared<AsyncFileInputStreamState>(
          std::move(file), chunk_size, start_offset)) {}

AsyncFileInputStream::~AsyncFileInputStream() {
  Close();
}

void AsyncFileInputStream::ReadAsync(scoped_refptr<IOBuffer> buf,
                                     std::size_t buf_len,
                                     IOReadCallback callback) {
  // Validate and prepare.
  AsyncFile* file = nullptr;
  std::int64_t offset = 0;
  std::size_t read_size = 0;

  {
    std::lock_guard<std::mutex> lock(state_->lock);
    if (state_->closed || !state_->file) {
      if (callback) callback(false, 0u);
      return;
    }
    if (state_->read_in_flight) {
      // Concurrent ReadAsync calls are not supported; treat as error.
      if (callback) callback(false, 0u);
      return;
    }
    state_->read_in_flight = true;
    file = state_->file.get();
    offset = state_->offset;
    // Honor whichever limit is smaller: what the caller provided vs the
    // chunk_size the stream was configured with.
    read_size = (std::min)(buf_len, state_->chunk_size);
  }

  // Issue the read.  AsyncFile::AsyncRead() delivers data as
  // std::vector<uint8_t>.  We copy it into the caller-supplied IOBuffer at
  // this boundary so that the rest of the stack works with the unified
  // IOBuffer abstraction.
  //
  // The buf scoped_refptr is captured so the buffer stays alive across the
  // asynchronous gap, even if the caller discards its handle.
  const bool accepted = file->AsyncRead(
      offset, read_size,
      [state = state_, buf, callback = std::move(callback)](
          bool success, std::vector<std::uint8_t>&& data,
          std::uint32_t /*error_code*/) mutable {
        std::size_t bytes_read = 0;
        bool ok = false;

        {
          std::lock_guard<std::mutex> lock(state->lock);
          state->read_in_flight = false;

          if (!state->closed && success && !data.empty()) {
            bytes_read = data.size();
            // Copy file data into the caller's IOBuffer region.
            std::memcpy(buf->data(), data.data(), bytes_read);
            state->offset += static_cast<std::int64_t>(bytes_read);
            ok = true;
          }
          // success=true but data.empty() means EOF; ok stays false.
        }

        if (callback) {
          callback(ok, bytes_read);
        }
      });

  if (!accepted) {
    // AsyncRead refused immediately (e.g., file already closed internally).
    std::lock_guard<std::mutex> lock(state_->lock);
    state_->read_in_flight = false;
    if (callback) callback(false, 0u);
  }
}

void AsyncFileInputStream::Close() {
  AsyncFile* file = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_->lock);
    if (state_->closed) {
      return;
    }
    state_->closed = true;
    file = state_->file.get();
  }

  if (file) {
    file->Close();
  }
}

}  // namespace nei