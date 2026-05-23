#include <neixx/io/async_line_reader.h>

#include <mutex>
#include <utility>

namespace nei {

namespace {

constexpr std::size_t kTextBufferCompactionThreshold = 2048;

}  // namespace

struct AsyncLineReader::State {
  std::mutex lock;
  LineCallback line_callback;
  std::string text_buffer;
  std::size_t consume_offset = 0;
  bool started = false;
};

AsyncLineReader::AsyncLineReader(AsyncInputStream* input_stream)
    : stream_(input_stream), state_(std::make_shared<State>()) {}

AsyncLineReader::~AsyncLineReader() {
  {
    std::lock_guard<std::mutex> lock(state_->lock);
    if (!state_->started) {
      return;
    }
    // Destructor must remain deterministic and non-reentrant. Never invoke
    // user callbacks from destruction context.
    state_->text_buffer.clear();
    state_->consume_offset = 0;
    state_->line_callback = LineCallback();
    state_->started = false;
  }
}

void AsyncLineReader::StartReadingLines(LineCallback callback) {
  {
    std::lock_guard<std::mutex> lock(state_->lock);
    if (state_->started || stream_ == nullptr) {
      return;
    }
    state_->started = true;
    state_->line_callback = std::move(callback);
  }

  const std::shared_ptr<State> state = state_;
  stream_->ReadAsync([state](std::vector<std::uint8_t>&& data) {
    OnRawDataReceived(state, std::move(data));
  });
}

void AsyncLineReader::FlushPendingLine() {
  LineCallback callback;
  std::string pending_line;
  {
    std::lock_guard<std::mutex> lock(state_->lock);
    if (!state_->started || !state_->line_callback) {
      return;
    }
    if (state_->consume_offset >= state_->text_buffer.size()) {
      return;
    }

    callback = state_->line_callback;
    pending_line.assign(state_->text_buffer.data() + state_->consume_offset,
                        state_->text_buffer.size() - state_->consume_offset);
    state_->text_buffer.clear();
    state_->consume_offset = 0;
  }

  if (callback && !pending_line.empty()) {
    callback(std::move(pending_line));
  }
}

void AsyncLineReader::OnRawDataReceived(const std::shared_ptr<State>& state,
                                        std::vector<std::uint8_t>&& data) {
  LineCallback callback;
  std::vector<std::string> completed_lines;

  // Convention: empty chunk means stream EOF/closed.
  {
    std::lock_guard<std::mutex> lock(state->lock);
    if (!state->started || !state->line_callback) {
      return;
    }

    callback = state->line_callback;

    if (data.empty()) {
      if (state->consume_offset < state->text_buffer.size()) {
        completed_lines.emplace_back(
            state->text_buffer.data() + state->consume_offset,
            state->text_buffer.size() - state->consume_offset);
      }
      state->text_buffer.clear();
      state->consume_offset = 0;
      state->line_callback = LineCallback();
      state->started = false;
    } else {
      state->text_buffer.append(reinterpret_cast<const char*>(data.data()),
                                data.size());

      std::size_t search_pos = state->consume_offset;
      while (true) {
        const std::size_t newline_pos = state->text_buffer.find('\n', search_pos);
        if (newline_pos == std::string::npos) {
          break;
        }

        const std::size_t line_start = state->consume_offset;
        std::size_t line_end = newline_pos;
        if (line_end > line_start && state->text_buffer[line_end - 1] == '\r') {
          --line_end;
        }

        completed_lines.emplace_back(state->text_buffer.data() + line_start,
                                     line_end - line_start);

        state->consume_offset = newline_pos + 1;
        search_pos = state->consume_offset;
      }

      if (state->consume_offset >= kTextBufferCompactionThreshold) {
        state->text_buffer.erase(0, state->consume_offset);
        state->consume_offset = 0;
      }

      if (state->consume_offset == state->text_buffer.size()) {
        state->text_buffer.clear();
        state->consume_offset = 0;
      }
    }
  }

  if (!callback) {
    return;
  }

  for (std::string& line : completed_lines) {
    callback(std::move(line));
  }
}

}  // namespace nei
