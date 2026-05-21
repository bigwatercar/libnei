#include <neixx/io/async_line_reader.h>

#include <mutex>
#include <utility>

namespace nei {

struct AsyncLineReader::State {
  std::mutex lock;
  LineCallback line_callback;
  std::string text_buffer;
  bool started = false;
};

AsyncLineReader::AsyncLineReader(AsyncInputStream* input_stream)
    : stream_(input_stream), state_(std::make_shared<State>()) {}

AsyncLineReader::~AsyncLineReader() {
  LineCallback callback;
  std::string pending_line;
  {
    std::lock_guard<std::mutex> lock(state_->lock);
    if (!state_->started) {
      return;
    }
    callback = state_->line_callback;
    pending_line = std::move(state_->text_buffer);
    state_->line_callback = LineCallback();
    state_->started = false;
  }

  if (!pending_line.empty() && callback) {
    callback(std::move(pending_line));
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
      if (!state->text_buffer.empty()) {
        completed_lines.push_back(std::move(state->text_buffer));
        state->text_buffer.clear();
      }
      state->line_callback = LineCallback();
      state->started = false;
    } else {
      state->text_buffer.append(reinterpret_cast<const char*>(data.data()),
                                data.size());

      std::size_t newline_pos = std::string::npos;
      while ((newline_pos = state->text_buffer.find('\n')) != std::string::npos) {
        std::string line = state->text_buffer.substr(0, newline_pos);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        completed_lines.push_back(std::move(line));
        state->text_buffer.erase(0, newline_pos + 1);
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
