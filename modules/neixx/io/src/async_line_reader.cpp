#include <neixx/io/async_line_reader.h>

#include <cstring>
#include <utility>
#include <vector>

#include <neixx/io/io_buffer.h>

namespace nei {

namespace {

// Each ReadAsync chunk is at most this many bytes.  4 KiB matches the
// IOBufferPool small-bucket size so AcquireBuffer() typically returns a
// recycled buffer with no heap allocation on the hot path.
constexpr std::size_t kChunkSize = 4096;

// Compact the internal text_buffer once the consumed prefix exceeds this
// threshold to prevent unbounded memory growth.
constexpr std::size_t kTextBufferCompactionThreshold = 2048;

}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// All fields are accessed only from the thread that owns the underlying
// stream (the IO thread / message-pump thread), so no mutex is required.
struct AsyncLineReader::State {
  AsyncInputStream* stream = nullptr;
  LineCallback line_callback;
  std::string text_buffer;
  std::size_t consume_offset = 0;
  bool started = false;
};

// ---------------------------------------------------------------------------
// AsyncLineReader
// ---------------------------------------------------------------------------

AsyncLineReader::AsyncLineReader(AsyncInputStream* input_stream)
    : stream_(input_stream), state_(std::make_shared<State>()) {
  state_->stream = input_stream;
}

AsyncLineReader::~AsyncLineReader() {
  state_->line_callback = LineCallback();
  state_->started = false;
}

void AsyncLineReader::StartReadingLines(LineCallback callback) {
  if (state_->started || stream_ == nullptr) return;
  state_->started = true;
  state_->line_callback = std::move(callback);
  IssueNextRead(state_);
}

void AsyncLineReader::FlushPendingLine() {
  if (!state_->started || !state_->line_callback) return;
  if (state_->consume_offset >= state_->text_buffer.size()) return;

  std::string pending_line(
      state_->text_buffer.data() + state_->consume_offset,
      state_->text_buffer.size() - state_->consume_offset);
  state_->text_buffer.clear();
  state_->consume_offset = 0;

  if (!pending_line.empty()) {
    state_->line_callback(std::move(pending_line));
  }
}

// static
void AsyncLineReader::IssueNextRead(const std::shared_ptr<State>& state) {
  if (!state->started || !state->line_callback || state->stream == nullptr) {
    return;
  }

  // Acquire a 4 KiB IOBuffer from the pool.  scoped_refptr does not support
  // implicit upcasting, so hold the concrete IOBufferWithSize reference and
  // construct an IOBuffer scoped_refptr from its raw pointer explicitly.
  scoped_refptr<IOBufferWithSize> sized_buf =
      IOBufferPool::GetInstance().AcquireBuffer(kChunkSize);
  scoped_refptr<IOBuffer> buf(sized_buf.get());

  // The lambda captures buf by value (scoped_refptr) so the buffer region
  // stays alive until the callback fires, even if the caller discards its
  // reference.
  state->stream->ReadAsync(
      buf, kChunkSize,
      [state, buf](bool ok, std::size_t bytes_read) mutable {
        // Append the raw bytes into the line-parsing text buffer BEFORE
        // calling the parse helper so that OnChunkReceived can work with
        // state->text_buffer directly.
        if (ok && bytes_read > 0) {
          state->text_buffer.append(buf->data(), bytes_read);
        }
        // Release our ref on buf now; the pool recycle hook (if set) will
        // return it automatically via IOBufferWithSize::~IOBufferWithSize().
        buf.reset();

        OnChunkReceived(state, ok, bytes_read);
      });
}

// static
void AsyncLineReader::OnChunkReceived(const std::shared_ptr<State>& state,
                                       bool ok,
                                       std::size_t bytes_read) {
  if (!state->started || !state->line_callback) return;

  if (!ok || bytes_read == 0) {
    // EOF or error.  Flush any remaining partial line.
    if (state->consume_offset < state->text_buffer.size()) {
      std::string trailing(
          state->text_buffer.data() + state->consume_offset,
          state->text_buffer.size() - state->consume_offset);
      state->text_buffer.clear();
      state->consume_offset = 0;
      if (!trailing.empty() && state->line_callback) {
        state->line_callback(std::move(trailing));
      }
    }
    state->line_callback = LineCallback();
    state->started = false;
    return;
  }

  // Parse complete lines from text_buffer[consume_offset..end].
  std::vector<std::string> completed_lines;
  std::size_t search_pos = state->consume_offset;
  while (true) {
    const std::size_t nl = state->text_buffer.find('\n', search_pos);
    if (nl == std::string::npos) break;

    const std::size_t line_start = state->consume_offset;
    std::size_t line_end = nl;
    // Strip trailing CR for CRLF line endings.
    if (line_end > line_start && state->text_buffer[line_end - 1] == '\r') {
      --line_end;
    }
    completed_lines.emplace_back(state->text_buffer.data() + line_start,
                                 line_end - line_start);
    state->consume_offset = nl + 1;
    search_pos = state->consume_offset;
  }

  // Compact the prefix already consumed to avoid unbounded growth.
  if (state->consume_offset >= kTextBufferCompactionThreshold) {
    state->text_buffer.erase(0, state->consume_offset);
    state->consume_offset = 0;
  }
  if (state->consume_offset == state->text_buffer.size()) {
    state->text_buffer.clear();
    state->consume_offset = 0;
  }

  // Deliver completed lines to the caller.
  for (std::string& line : completed_lines) {
    if (state->line_callback) {
      state->line_callback(std::move(line));
    }
  }

  // Issue the next read if still active.
  if (state->started && state->line_callback) {
    IssueNextRead(state);
  }
}

}  // namespace nei