#include <neixx/io/async_file_input_stream.h>

#include <algorithm>
#include <mutex>
#include <utility>

namespace nei {

// ---------------------------------------------------------------------------
// AsyncFileInputStreamState
// ---------------------------------------------------------------------------
//
// Pull model: each ReadAsync() call issues exactly one AsyncFile::ReadAsync()
// and fires exactly one callback.  The caller is responsible for issuing the
// next ReadAsync() if it wants more data.  This matches the AsyncInputStream
// pull contract agreed by all concrete implementations in this refactor.
struct AsyncFileInputStreamState {
  explicit AsyncFileInputStreamState(std::unique_ptr<AsyncFile> file_in,
                                     std::size_t chunk_size_in,
                                     std::int64_t start_offset)
      : file(std::move(file_in)),
        chunk_size((std::max)(chunk_size_in, static_cast<std::size_t>(1))),
        next_offset(start_offset < 0 ? 0 : static_cast<std::uint64_t>(start_offset)) {}

  std::mutex lock;
  std::unique_ptr<AsyncFile> file;
  std::size_t chunk_size = 4096;
  std::uint64_t next_offset = 0;
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
  std::size_t read_size = 0;
  std::uint64_t offset = 0;

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
    offset = state_->next_offset;
    // Honor whichever limit is smaller: what the caller provided vs the
    // chunk_size the stream was configured with.
    read_size = (std::min)(buf_len, state_->chunk_size);
  }

  // Issue the read directly into the caller-supplied IOBuffer.
  file->ReadAsync(
      std::move(buf), read_size, offset,
      [state = state_, callback = std::move(callback)](
          bool success, std::size_t bytes_read,
          AsyncFile::Error /*error*/) mutable {
        bool ok = false;

        {
          std::lock_guard<std::mutex> lock(state->lock);
          state->read_in_flight = false;
          if (success && bytes_read > 0) {
            state->next_offset += static_cast<std::uint64_t>(bytes_read);
          }
          ok = (!state->closed && success && bytes_read > 0);
        }

        if (callback) {
          callback(ok, bytes_read);
        }
      });
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
    file->CloseAsync(nullptr);
  }
}

}  // namespace nei