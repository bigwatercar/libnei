#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <neixx/io/async_line_reader.h>
#include <neixx/io/async_stream.h>

namespace nei {
namespace {

class FakeAsyncInputStream final : public AsyncInputStream {
 public:
  void ReadAsync(DataCallback callback) override { callback_ = std::move(callback); }

  void Close() override {
    if (callback_) {
      callback_({});
    }
  }

  void Emit(const std::string& chunk) {
    if (!callback_) {
      return;
    }
    std::vector<std::uint8_t> data(chunk.begin(), chunk.end());
    callback_(std::move(data));
  }

  void EmitEof() {
    if (callback_) {
      callback_({});
    }
  }

 private:
  DataCallback callback_;
};

TEST(AsyncLineReaderTest, SplitsLinesAndTrimsCarriageReturn) {
  FakeAsyncInputStream input;
  AsyncLineReader reader(&input);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

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
    // Reader is destroyed without EOF.
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
