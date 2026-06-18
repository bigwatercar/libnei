#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_file.h>
#include <neixx/io/file_stream_adapters.h>
#include <neixx/io/async_line_reader.h>
#include <neixx/io/io_buffer.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

std::filesystem::path MakeTempFilePath(const char* name_hint) {
  const auto ticks =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::filesystem::path p = std::filesystem::temp_directory_path();
  p /= std::string("nei_file_stream_adapter_") + name_hint + "_" +
       std::to_string(static_cast<long long>(ticks)) + ".txt";
  return p;
}

TEST(FileInputStreamAdapterTest, BridgesAsyncFileAndAsyncLineReader) {
  Thread io_thread("file-stream-adapter-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread background_thread("file-stream-adapter-bg");
  ASSERT_TRUE(background_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> bg_runner = background_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(bg_runner);

  const std::filesystem::path path = MakeTempFilePath("line_bridge");
  const std::string path_utf8 = path.u8string();

  auto writer_file = AsyncFile::Create(io_runner);
  ASSERT_TRUE(writer_file);

  WaitableEvent write_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write_close_barrier(WaitableEvent::ResetPolicy::kAutomatic,
                                    false);
  std::atomic<bool> write_open_ok{false};
  std::atomic<bool> write_ok{false};

  writer_file->OpenAsync(
      path_utf8, AsyncFile::OpenMode::kReadWrite,
      AsyncFile::OpenDisposition::kCreateAlways, bg_runner,
      [&](bool success, AsyncFile::Error error) {
        write_open_ok.store(success && error.ok(),
                            std::memory_order_release);
        write_open_done.Signal();
      });

  ASSERT_TRUE(write_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write_open_ok.load(std::memory_order_acquire));

  const std::string text = "alpha\nbeta\ngamma\r\nlast_line";
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  scoped_refptr<IOBuffer> write_buf(new IOBufferWithSize(bytes.size()));
  std::memcpy(write_buf->data(), bytes.data(), bytes.size());
  writer_file->WriteAsync(
      write_buf, bytes.size(), 0,
      [&](bool success, std::size_t wrote, AsyncFile::Error error) {
        write_ok.store(success && error.ok() && wrote == bytes.size(),
                       std::memory_order_release);
        write_done.Signal();
      });
  ASSERT_TRUE(write_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write_ok.load(std::memory_order_acquire));

  writer_file->Close([&]() { write_close_barrier.Signal(); });  ASSERT_TRUE(write_close_barrier.TimedWait(std::chrono::seconds(10)));

  auto reader_file = AsyncFile::Create(io_runner);
  ASSERT_TRUE(reader_file);

  WaitableEvent read_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_close_barrier(WaitableEvent::ResetPolicy::kAutomatic,
                                   false);
  std::atomic<bool> read_open_ok{false};

  reader_file->OpenAsync(
      path_utf8, AsyncFile::OpenMode::kReadOnly,
      AsyncFile::OpenDisposition::kOpenExisting, bg_runner,
      [&](bool success, AsyncFile::Error error) {
        read_open_ok.store(success && error.ok(),
                           std::memory_order_release);
        read_open_done.Signal();
      });
  ASSERT_TRUE(read_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_open_ok.load(std::memory_order_acquire));

  // FileInputStreamAdapter borrows the AsyncFile* (caller retains ownership).
  FileInputStreamAdapter input_stream(reader_file.get(), io_runner);
  AsyncLineReader line_reader(&input_stream);

  WaitableEvent lines_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::vector<std::string> lines;

  line_reader.StartReadingLines([&](std::string&& line) {
    lines.push_back(std::move(line));
    if (lines.size() == 4u) {
      lines_done.Signal();
    }
  });

  ASSERT_TRUE(lines_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[0], "alpha");
  EXPECT_EQ(lines[1], "beta");
  EXPECT_EQ(lines[2], "gamma");
  EXPECT_EQ(lines[3], "last_line");

  input_stream.Close();
  io_runner->PostTask(FROM_HERE, [&]() { read_close_barrier.Signal(); });
  ASSERT_TRUE(read_close_barrier.TimedWait(std::chrono::seconds(10)));

  background_thread.Stop();
  io_thread.Stop();

  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

}  // namespace
}  // namespace nei
