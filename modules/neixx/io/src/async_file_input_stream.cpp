#include <neixx/io/async_file_input_stream.h>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace nei {

struct AsyncFileInputStreamState {
  explicit AsyncFileInputStreamState(std::unique_ptr<AsyncFile> file_in,
                                     std::size_t chunk_size_in,
                                     std::int64_t start_offset)
      : file(std::move(file_in)),
        chunk_size((std::max)(chunk_size_in, static_cast<std::size_t>(1))),
        offset(start_offset) {}

  std::mutex lock;
  std::unique_ptr<AsyncFile> file;
  AsyncInputStream::DataCallback callback;
  std::size_t chunk_size = 4096;
  std::int64_t offset = 0;
  bool started = false;
  bool closed = false;
  bool read_in_flight = false;
  bool eof_signaled = false;
};

namespace {

void ScheduleNextRead(const std::shared_ptr<AsyncFileInputStreamState>& state);

void EmitEof(const AsyncFileInputStream::DataCallback& callback) {
  if (callback) {
    callback({});
  }
}

void OnReadCompleted(const std::shared_ptr<AsyncFileInputStreamState>& state,
                     bool success,
                     std::vector<std::uint8_t>&& data) {
  AsyncFileInputStream::DataCallback callback;
  std::vector<std::uint8_t> payload;
  bool emit_eof = false;
  bool schedule_next = false;

  {
    std::lock_guard<std::mutex> lock(state->lock);
    state->read_in_flight = false;

    if (!state->started || !state->callback) {
      return;
    }

    callback = state->callback;

    if (state->closed || !success) {
      state->started = false;
      state->eof_signaled = true;
      state->callback = AsyncFileInputStream::DataCallback();
      emit_eof = true;
    } else if (data.empty()) {
      state->started = false;
      state->eof_signaled = true;
      state->callback = AsyncFileInputStream::DataCallback();
      emit_eof = true;
    } else {
      state->offset += static_cast<std::int64_t>(data.size());
      payload = std::move(data);
      schedule_next = true;
    }
  }

  if (!payload.empty() && callback) {
    callback(std::move(payload));
  }
  if (emit_eof) {
    EmitEof(callback);
  }
  if (schedule_next) {
    ScheduleNextRead(state);
  }
}

void ScheduleNextRead(const std::shared_ptr<AsyncFileInputStreamState>& state) {
  AsyncFile* file = nullptr;
  std::int64_t offset = 0;
  std::size_t chunk_size = 0;

  {
    std::lock_guard<std::mutex> lock(state->lock);
    if (!state->started || state->closed || !state->callback || !state->file ||
        state->read_in_flight) {
      return;
    }

    file = state->file.get();
    offset = state->offset;
    chunk_size = state->chunk_size;
    state->read_in_flight = true;
  }

  const bool accepted = file->AsyncRead(
      offset, chunk_size,
      [state](bool success, std::vector<std::uint8_t>&& data,
              std::uint32_t /*error_code*/) {
        OnReadCompleted(state, success, std::move(data));
      });

  if (!accepted) {
    OnReadCompleted(state, false, {});
  }
}

void CloseInternal(const std::shared_ptr<AsyncFileInputStreamState>& state,
                   bool emit_eof) {
  AsyncFileInputStream::DataCallback callback;
  AsyncFile* file = nullptr;

  {
    std::lock_guard<std::mutex> lock(state->lock);
    if (state->closed) {
      return;
    }

    state->closed = true;
    state->started = false;
    file = state->file.get();

    if (emit_eof && state->callback && !state->eof_signaled) {
      callback = state->callback;
      state->eof_signaled = true;
    }

    state->callback = AsyncFileInputStream::DataCallback();
  }

  if (file) {
    file->Close();
  }

  if (callback) {
    EmitEof(callback);
  }
}

}  // namespace

AsyncFileInputStream::AsyncFileInputStream(std::unique_ptr<AsyncFile> file,
                                           std::size_t chunk_size,
                                           std::int64_t start_offset)
  : state_(std::make_shared<AsyncFileInputStreamState>(
      std::move(file), chunk_size, start_offset)) {}

AsyncFileInputStream::~AsyncFileInputStream() {
  CloseInternal(state_, false);
}

void AsyncFileInputStream::ReadAsync(DataCallback callback) {
  {
    std::lock_guard<std::mutex> lock(state_->lock);
    if (state_->started || state_->closed || !state_->file) {
      return;
    }
    state_->started = true;
    state_->callback = std::move(callback);
  }

  ScheduleNextRead(state_);
}

void AsyncFileInputStream::Close() {
  CloseInternal(state_, true);
}

}  // namespace nei
