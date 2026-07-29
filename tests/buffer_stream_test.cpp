// ===========================================================================
// BufferInputStream tests
// ===========================================================================

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <neixx/io/async_line_reader.h>
#include <neixx/io/buffer_stream.h>
#include <neixx/io/io_buffer.h>

namespace nei {
namespace {

// ---------------------------------------------------------------------------
// Standalone BufferInputStream tests
// ---------------------------------------------------------------------------

TEST(BufferInputStreamTest, ReadBytesFromString) {
  std::string data = "Hello, World!";
  BufferInputStream stream(std::move(data));

  auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(64));
  bool called = false;
  stream.ReadAsync(buf, 64, [&](bool ok, std::size_t n) {
    called = true;
    EXPECT_TRUE(ok);
    EXPECT_EQ(n, 13u);
    EXPECT_EQ(std::memcmp(buf->data(), "Hello, World!", 13), 0);
  });
  EXPECT_TRUE(called);
}

TEST(BufferInputStreamTest, MultipleReadsAdvanceCursor) {
  std::string data = "0123456789";
  BufferInputStream stream(std::move(data));

  auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(4));
  int call_count = 0;
  std::string received;

  // Read 4, 4, 2 bytes  --  should exhaust the 10-byte buffer.
  for (int expected : {4, 4, 2}) {
    stream.ReadAsync(buf, 4, [&](bool ok, std::size_t n) {
      call_count++;
      EXPECT_TRUE(ok);
      EXPECT_EQ(n, static_cast<std::size_t>(expected));
      received.append(reinterpret_cast<const char*>(buf->data()), n);
    });
  }

  EXPECT_EQ(call_count, 3);
  EXPECT_EQ(received, "0123456789");
}

TEST(BufferInputStreamTest, ReadPastEOFReturnsZero) {
  std::string data = "abc";
  BufferInputStream stream(std::move(data));

  auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(16));
  bool first_ok = false, second_eof = false;

  stream.ReadAsync(buf, 16, [&](bool ok, std::size_t n) {
    first_ok = ok;
    EXPECT_EQ(n, 3u);
  });

  stream.ReadAsync(buf, 16, [&](bool ok, std::size_t n) {
    second_eof = !ok && n == 0;
  });

  EXPECT_TRUE(first_ok);
  EXPECT_TRUE(second_eof);
}

TEST(BufferInputStreamTest, ClosePreventsFurtherReads) {
  std::string data = "data";
  BufferInputStream stream(std::move(data));

  stream.Close();

  auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(16));
  bool saw_eof = false;
  stream.ReadAsync(buf, 16, [&](bool ok, std::size_t n) {
    saw_eof = !ok && n == 0;
  });
  EXPECT_TRUE(saw_eof);
}

TEST(BufferInputStreamTest, SizeAndRemaining) {
  std::string data = "1234567890";  // 10 bytes
  BufferInputStream stream(std::move(data));

  EXPECT_EQ(stream.size(), 10u);
  EXPECT_EQ(stream.remaining(), 10u);

  auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(3));
  stream.ReadAsync(buf, 3, [](bool ok, std::size_t n) {
    EXPECT_TRUE(ok);
    EXPECT_EQ(n, 3u);
  });

  EXPECT_EQ(stream.remaining(), 7u);
  EXPECT_EQ(stream.size(), 10u);  // size never changes
}

TEST(BufferInputStreamTest, BorrowFromRawPointer) {
  const char* text = "borrowed data";
  BufferInputStream stream(text, std::strlen(text));

  auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(64));
  stream.ReadAsync(buf, 64, [&](bool ok, std::size_t n) {
    EXPECT_TRUE(ok);
    EXPECT_EQ(n, std::strlen(text));
    EXPECT_EQ(std::memcmp(buf->data(), text, n), 0);
  });
}

TEST(BufferInputStreamTest, BorrowFromIOBuffer) {
  auto source = scoped_refptr<IOBufferWithSize>(new IOBufferWithSize(5));
  std::memcpy(source->data(), "12345", 5);
  // Pass as IOBuffer (scoped_refptr<IOBufferWithSize> -> scoped_refptr<IOBuffer>
  // not implicit  --  use raw pointer).
  BufferInputStream stream(
      scoped_refptr<IOBuffer>(source.get()), 5);

  auto buf = scoped_refptr<IOBuffer>(new IOBufferWithSize(16));
  stream.ReadAsync(buf, 16, [&](bool ok, std::size_t n) {
    EXPECT_TRUE(ok);
    EXPECT_EQ(n, 5u);
  });
}

// ---------------------------------------------------------------------------
// BufferInputStream + AsyncLineReader integration tests
// ---------------------------------------------------------------------------

TEST(BufferInputStreamTest, ParseLinesWithAsyncLineReader) {
  std::string data = "line1\nline2\nline3\n";
  BufferInputStream stream(std::move(data));
  AsyncLineReader reader(&stream);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "line1");
  EXPECT_EQ(lines[1], "line2");
  EXPECT_EQ(lines[2], "line3");
}

TEST(BufferInputStreamTest, ParseLinesEmptyBuffer) {
  BufferInputStream stream(std::string(""));
  AsyncLineReader reader(&stream);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  EXPECT_TRUE(lines.empty());
}

TEST(BufferInputStreamTest, ParseLinesWithoutTrailingNewline) {
  // When the stream signals EOF, AsyncLineReader automatically delivers
  // the remaining text as a final line even without a trailing newline.
  std::string data = "hello\nworld";
  BufferInputStream stream(std::move(data));
  AsyncLineReader reader(&stream);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  // Both "hello" (terminated by \n) and "world" (terminated by EOF)
  // are delivered automatically.
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "hello");
  EXPECT_EQ(lines[1], "world");

  // FlushPendingLine is a no-op when already drained by EOF.
  std::size_t before = lines.size();
  reader.FlushPendingLine();
  EXPECT_EQ(lines.size(), before);
}

TEST(BufferInputStreamTest, ParseLinesOnlyPartialLine) {
  // Buffer has data but NO newline at all.
  // AsyncLineReader delivers it automatically when the stream hits EOF.
  std::string data = "incomplete";
  BufferInputStream stream(std::move(data));
  AsyncLineReader reader(&stream);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  // Delivered as a complete line by EOF semantics.
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "incomplete");

  // FlushPendingLine is a no-op after EOF drain.
  std::size_t before = lines.size();
  reader.FlushPendingLine();
  EXPECT_EQ(lines.size(), before);
}

TEST(BufferInputStreamTest, ParseLinesEmptyFlushIsNoop) {
  std::string data = "done\n";
  BufferInputStream stream(std::move(data));
  AsyncLineReader reader(&stream);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "done");

  // Flush with nothing pending should be a no-op (no extra callback).
  reader.FlushPendingLine();
  EXPECT_EQ(lines.size(), 1u);
}

TEST(BufferInputStreamTest, ParseLinesCRLFHandling) {
  // AsyncLineReader strips trailing \r before \n.
  std::string data = "line1\r\nline2\r\nline3\r\n";
  BufferInputStream stream(std::move(data));
  AsyncLineReader reader(&stream);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "line1");
  EXPECT_EQ(lines[1], "line2");
  EXPECT_EQ(lines[2], "line3");
}

TEST(BufferInputStreamTest, ParseLinesEmptyLines) {
  std::string data = "\n\n\n";
  BufferInputStream stream(std::move(data));
  AsyncLineReader reader(&stream);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  ASSERT_EQ(lines.size(), 3u);
  EXPECT_TRUE(lines[0].empty());
  EXPECT_TRUE(lines[1].empty());
  EXPECT_TRUE(lines[2].empty());
}

TEST(BufferInputStreamTest, ParseLinesMixedEmptyLines) {
  std::string data = "a\n\nb\n\n";
  BufferInputStream stream(std::move(data));
  AsyncLineReader reader(&stream);

  std::vector<std::string> lines;
  reader.StartReadingLines([&lines](std::string&& line) {
    lines.push_back(std::move(line));
  });

  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[0], "a");
  EXPECT_TRUE(lines[1].empty());
  EXPECT_EQ(lines[2], "b");
  EXPECT_TRUE(lines[3].empty());
}

}  // namespace
}  // namespace nei
