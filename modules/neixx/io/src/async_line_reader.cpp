#include <neixx/io/async_line_reader.h>

#include <utility>

namespace nei {

AsyncLineReader::AsyncLineReader(AsyncInputStream* input_stream)
    : stream_(input_stream) {}

AsyncLineReader::~AsyncLineReader() {
  if (!started_) {
    return;
  }
  if (!text_buffer_.empty() && line_callback_) {
    line_callback_(std::move(text_buffer_));
  }
}

void AsyncLineReader::StartReadingLines(LineCallback callback) {
  if (started_ || stream_ == nullptr) {
    return;
  }
  started_ = true;
  line_callback_ = std::move(callback);
  stream_->ReadAsync([this](std::vector<std::uint8_t>&& data) {
    OnRawDataReceived(std::move(data));
  });
}

void AsyncLineReader::OnRawDataReceived(std::vector<std::uint8_t>&& data) {
  // Convention: empty chunk means stream EOF/closed.
  if (data.empty()) {
    if (!text_buffer_.empty() && line_callback_) {
      line_callback_(std::move(text_buffer_));
      text_buffer_.clear();
    }
    return;
  }

  text_buffer_.append(reinterpret_cast<const char*>(data.data()), data.size());

  std::size_t newline_pos = std::string::npos;
  while ((newline_pos = text_buffer_.find('\n')) != std::string::npos) {
    std::string line = text_buffer_.substr(0, newline_pos);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line_callback_) {
      line_callback_(std::move(line));
    }
    text_buffer_.erase(0, newline_pos + 1);
  }
}

}  // namespace nei
