#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <neixx/io/async_line_reader.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>

namespace nei {
namespace {

// FakeAsyncInputStream implements the pull model:
//   ReadAsync() stores the pending (buf, buf_len, callback) tuple.
//   Emit()     writes bytes into the stored buf and fires the callback.
//   EmitEof()  fires the callback with (false, 0) to signal EOF.
//
// This mirrors the real pipe/file stream pull contract so that AsyncLineReader
// tests remain faithful to production behaviour.
class FakeAsyncInputStream final : public AsyncInputStream {
 public:
  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override {
    pending_buf_ = std::move(buf);
    pending_len_ = buf_len;
    pending_cb_ = std::move(callback);
  }

  void Close() override {
    EmitEof();
  }

  // Push `chunk` bytes into the pending read buffer and fire the callback.
  void Emit(const std::string& chunk) {
    if (!pending_cb_) return;

    const std::size_t n = (std::min)(chunk.size(), pending_len_);
    std::memcpy(pending_buf_->data(), chunk.data(), n);

    IOReadCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    cb(true, n);
  }

  // Signal EOF: fire the pending callback with success=false, bytes=0.
  void EmitEof() {
    if (!pending_cb_) return;
    IOReadCallback cb = std::move(pending_cb_);
    pending_buf_.reset();
    cb(false, 0u);
  }

 private:
  scoped_refptr<IOBuffer> pending_buf_;
  std::size_t pending_len_ = 0;
  IOReadCallback pending_cb_;
};

TEST(AsyncLineReaderTest, SplitsLinesAndTrimsCarriageReturn) {
  FakeAsyncInputStream input;
  AsyncLineReader reader(&input);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  // Each Emit() satisfies the most recent ReadAsync() and AsyncLineReader
  // issues the next ReadAsync() internally (pull model).
  input.Emit("first\r\nsecond\nthird");
  input.Emit("\r\n");
  input.EmitEof();

  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "first");
  EXPECT_EQ(lines[1], "second");
  EXPECT_EQ(lines[2], "third");
}

TEST(AsyncLineReaderTest, FlushesTrailingPartialLineOnEof) {
  FakeAsyncInputStream input;
  AsyncLineReader reader(&input);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  input.Emit("partial-without-newline");
  input.EmitEof();

  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "partial-without-newline");
}

TEST(AsyncLineReaderTest, DestructorDoesNotInvokeCallback) {
  FakeAsyncInputStream input;
  std::vector<std::string> lines;
  {
    AsyncLineReader reader(&input);
    reader.StartReadingLines([&lines](std::string&& line) {
      lines.push_back(std::move(line));
    });

    input.Emit("trailing-without-eof");
    // Reader is destroyed without EOF.  The destructor must not call the
    // line callback.
  }

  EXPECT_TRUE(lines.empty());
}

TEST(AsyncLineReaderTest, FlushPendingLineEmitsTrailingDataAndKeepsReading) {
  FakeAsyncInputStream input;
  AsyncLineReader reader(&input);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  input.Emit("trailing");
  reader.FlushPendingLine();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "trailing");

  input.Emit("next\n");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1], "next");

  input.EmitEof();
  EXPECT_EQ(lines.size(), 2u);
}

}  // namespace
}  // namespace nei