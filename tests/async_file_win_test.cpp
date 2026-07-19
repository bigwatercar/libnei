#include <gtest/gtest.h>

#if defined(_WIN32)

#include <Windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_line_reader.h>
#include <neixx/io/async_stream.h>
#include <async_file_win.h>
#include <neixx/task/message_loop/message_pump_io.h>
#include <neixx/task/task_runner.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread.h>
#include <neixx/io/io_buffer.h>
#if __cplusplus >= 202002L
namespace { inline std::string PathToUTF8(const std::filesystem::path& p) { auto u = p.u8string(); return {reinterpret_cast<const char*>(u.data()), u.size()}; } }
#else
namespace { inline std::string PathToUTF8(const std::filesystem::path& p) { return p.u8string(); } }
#endif



namespace nei {
namespace {

std::filesystem::path MakeTempFilePath(const char* name_hint) {
  std::filesystem::path p = std::filesystem::temp_directory_path();
  p /= std::string("nei_async_file_win_") + name_hint + "_" +
       std::to_string(GetCurrentProcessId()) + "_" +
       std::to_string(GetTickCount64()) + ".bin";
  return p;
}

std::vector<std::uint8_t> MakePayload(std::size_t size, std::uint8_t salt) {
  std::vector<std::uint8_t> data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<std::uint8_t>((i * 131u + salt) & 0xFFu);
  }
  return data;
}

bool IssueWrite(const std::shared_ptr<AsyncFileWin>& file,
                std::uint64_t offset,
                std::vector<std::uint8_t> data,
                AsyncFile::WriteCallback callback) {
  if (!file || !callback) {
    return false;
  }
  scoped_refptr<IOBuffer> buf(new IOBufferWithSize(data.size()));
  if (!data.empty()) {
    std::memcpy(buf->data(), data.data(), data.size());
  }
  file->WriteAsync(buf, data.size(), offset, std::move(callback));
  return true;
}

bool IssueRead(
    const std::shared_ptr<AsyncFileWin>& file,
    std::uint64_t offset,
    std::size_t bytes_to_read,
    std::function<void(bool success,
                       std::vector<std::uint8_t>&& data,
                       AsyncFile::Error error)> callback) {
  if (!file || !callback) {
    return false;
  }
  scoped_refptr<IOBuffer> buf(new IOBufferWithSize(bytes_to_read));
  file->ReadAsync(
      buf, bytes_to_read, offset,
      [buf, callback = std::move(callback)](bool success,
                                            std::size_t bytes_read,
                                            AsyncFile::Error error) mutable {
        std::vector<std::uint8_t> out;
        if (bytes_read > 0) {
          out.resize(bytes_read);
          std::memcpy(out.data(), buf->data(), bytes_read);
        }
        callback(success, std::move(out), error);
      });
  return true;
}

class FileBackedAsyncInputStream final : public AsyncInputStream {
 public:
  FileBackedAsyncInputStream(std::shared_ptr<AsyncFileWin> file,
                             std::size_t chunk_size)
      : file_(std::move(file)), chunk_size_(chunk_size) {}

  void ReadAsync(scoped_refptr<IOBuffer> buf,
                 std::size_t buf_len,
                 IOReadCallback callback) override {
    if (closed_) {
      if (callback) callback(false, 0u);
      return;
    }
    buf_ = std::move(buf);
    buf_len_ = buf_len;
    callback_ = std::move(callback);
    ScheduleNextRead();
  }

  void Close() override {
    closed_ = true;
    callback_ = IOReadCallback();
    buf_.reset();
  }

 private:
  void ScheduleNextRead() {
    if (closed_ || !callback_ || !file_ || !buf_) {
      return;
    }

    const std::size_t read_size = (std::min)(buf_len_, chunk_size_);
    // Capture buf_ by value to keep it alive across the async gap.
    scoped_refptr<IOBuffer> buf = buf_;
    const bool accepted = IssueRead(
      file_,
        offset_, read_size,
        [this, buf](bool success, std::vector<std::uint8_t>&& data,
                    AsyncFile::Error /*error*/) mutable {
          if (closed_ || !callback_) {
            return;
          }

          if (!success) {
            IOReadCallback cb = std::move(callback_);
            buf_.reset();
            cb(false, 0u);
            return;
          }

          const std::size_t got = data.size();
          if (got == 0) {
            IOReadCallback cb = std::move(callback_);
            buf_.reset();
            cb(false, 0u);
            return;
          }

          // Copy data into caller's IOBuffer.
          std::memcpy(buf->data(), data.data(), got);
          offset_ += static_cast<std::int64_t>(got);

          IOReadCallback cb = std::move(callback_);
          buf_.reset();
          cb(true, got);
        });

    if (!accepted) {
      if (callback_) {
        IOReadCallback cb = std::move(callback_);
        buf_.reset();
        cb(false, 0u);
      }
    }
  }

  std::shared_ptr<AsyncFileWin> file_;
  IOReadCallback callback_;
  scoped_refptr<IOBuffer> buf_;
  std::size_t buf_len_ = 0;
  std::int64_t offset_ = 0;
  std::size_t chunk_size_ = 0;
  bool closed_ = false;
};

TEST(AsyncFileWinTest, OpenMissingFileReturnsGenericNoSuchFile) {
  Thread io_thread("async-file-win-open-missing-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread background_thread("async-file-win-open-missing-bg");
  ASSERT_TRUE(background_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> bg_runner = background_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(bg_runner);

  const std::filesystem::path path = MakeTempFilePath("missing_open");
  std::error_code remove_ec;
  (void)std::filesystem::remove(path, remove_ec);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> callback_called{false};
  std::atomic<bool> success{true};
  std::atomic<AsyncFile::ErrorCode> error_code{AsyncFile::ErrorCode::kOk};

  io_runner->PostTask(FROM_HERE, [&, io_runner, bg_runner]() {
    auto file = std::make_shared<AsyncFileWin>(io_runner);
    file->OpenAsync(PathToUTF8(path), AsyncFile::OpenMode::kReadOnly,
                    AsyncFile::OpenDisposition::kOpenExisting, bg_runner,
                    [&, file](bool open_success, AsyncFile::Error open_error) {
                      callback_called.store(true, std::memory_order_release);
                      success.store(open_success, std::memory_order_release);
                      error_code.store(open_error.code, std::memory_order_release);
                      done.Signal();
                    });
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(10)));
  EXPECT_TRUE(callback_called.load(std::memory_order_acquire));
  EXPECT_FALSE(success.load(std::memory_order_acquire));
  EXPECT_EQ(error_code.load(std::memory_order_acquire),
            AsyncFile::ErrorCode::kNotFound);

  background_thread.Stop();
  io_thread.Stop();
}

TEST(AsyncFileWinTest, LargeReadWriteCallbackDeterminismOnIoThread) {
  AsyncFileWin::ResetStageCountersForTesting();
  MessagePumpForIO::ResetDebugCountersForTesting();
  TaskRunner::ResetTracingStatsForTesting();

  Thread io_thread("async-file-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread background_thread("async-file-bg");
  ASSERT_TRUE(background_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> bg_runner = background_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(bg_runner);

  const std::filesystem::path path = MakeTempFilePath("large_det");
  const std::string path_utf8 = PathToUTF8(path);
  const std::size_t kLargeBytes = 32u * 1024u * 1024u;
  const std::vector<std::uint8_t> payload = MakePayload(kLargeBytes, 7);

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{true};
  std::atomic<bool> callback_on_io_thread{true};
  std::atomic<bool> callback_inline_violation{false};
  std::atomic<PlatformThread::PlatformThreadId> io_tid{0};

  io_runner->PostTask(FROM_HERE, [&, io_runner, bg_runner, path_utf8, payload]() mutable {
    io_tid.store(PlatformThread::CurrentId(), std::memory_order_release);
    auto file = std::make_shared<AsyncFileWin>(io_runner);

    auto open_called = std::make_shared<std::atomic<bool>>(false);
    file->OpenAsync(
        path_utf8, AsyncFile::OpenMode::kReadWrite,
        AsyncFile::OpenDisposition::kCreateAlways, bg_runner,
        [&, file, payload, open_called](bool open_success,
                                        AsyncFile::Error open_error) mutable {
          open_called->store(true, std::memory_order_release);
          if (PlatformThread::CurrentId() !=
              io_tid.load(std::memory_order_acquire)) {
            callback_on_io_thread.store(false, std::memory_order_release);
          }
          if (!open_success || !open_error.ok()) {
            ok.store(false, std::memory_order_release);
            file->CloseAsync([&]() { done.Signal(); });
            return;
          }

          auto write_called = std::make_shared<std::atomic<bool>>(false);
            const bool write_accepted = IssueWrite(
              file,
              0, payload,
              [&, file, payload, write_called](bool write_success,
                                               std::size_t bytes_written,
                                               AsyncFile::Error write_error) mutable {
                write_called->store(true, std::memory_order_release);
                if (PlatformThread::CurrentId() !=
                    io_tid.load(std::memory_order_acquire)) {
                  callback_on_io_thread.store(false, std::memory_order_release);
                }
                if (!write_success || !write_error.ok() ||
                    bytes_written != payload.size()) {
                  ok.store(false, std::memory_order_release);
                  file->CloseAsync([&]() { done.Signal(); });
                  return;
                }

                auto read_called = std::make_shared<std::atomic<bool>>(false);
                const bool read_accepted = IssueRead(
                  file,
                    0, payload.size(),
                    [&, file, payload, read_called](bool read_success,
                                                    std::vector<std::uint8_t>&& data,
                                                    AsyncFile::Error read_error) mutable {
                      read_called->store(true, std::memory_order_release);
                      if (PlatformThread::CurrentId() !=
                          io_tid.load(std::memory_order_acquire)) {
                        callback_on_io_thread.store(false,
                                                    std::memory_order_release);
                      }
                      if (!read_success || !read_error.ok() ||
                          data != payload) {
                        ok.store(false, std::memory_order_release);
                      }
                      file->CloseAsync([&]() { done.Signal(); });
                    });

                if (!read_accepted) {
                  ok.store(false, std::memory_order_release);
                  file->CloseAsync([&]() { done.Signal(); });
                  return;
                }
                if (read_called->load(std::memory_order_acquire)) {
                  callback_inline_violation.store(true,
                                                 std::memory_order_release);
                }
              });

          if (!write_accepted) {
            ok.store(false, std::memory_order_release);
            file->CloseAsync([&]() { done.Signal(); });
            return;
          }
          if (write_called->load(std::memory_order_acquire)) {
            callback_inline_violation.store(true, std::memory_order_release);
          }
        });

    if (open_called->load(std::memory_order_acquire)) {
      callback_inline_violation.store(true, std::memory_order_release);
    }
  });

  const bool completed = done.TimedWait(std::chrono::seconds(30));
  EXPECT_TRUE(completed);
  const AsyncFileWin::StageCounters counters =
      AsyncFileWin::GetStageCountersForTesting();
    const MessagePumpForIO::DebugCounters pump_counters =
      MessagePumpForIO::GetDebugCountersForTesting();
    const TaskRunnerTracingStats task_counters =
      TaskRunner::GetTracingStatsForTesting();
  if (!completed) {
    ADD_FAILURE() << "stage counters: open=" << counters.open_reached
                  << " write=" << counters.write_reached
                  << " read=" << counters.read_reached
                  << " iocp_completed=" << counters.iocp_completed
                  << " context_hit=" << counters.context_hit
                  << " context_miss=" << counters.context_miss
                  << " read_finalize_attempted="
                  << counters.read_finalize_attempted
                  << " read_posted=" << counters.read_posted
                  << " callback_weak_dropped="
                  << counters.callback_weak_dropped
                  << " callback_post_failed="
                  << counters.callback_post_failed
                  << " write_post_seq=" << counters.write_post_seq
                  << " write_exec_seq=" << counters.write_exec_seq
                  << " read_post_seq=" << counters.read_post_seq
                  << " read_exec_seq=" << counters.read_exec_seq
                  << " pump_do_work_calls="
                  << pump_counters.do_work_calls
                  << " pump_do_work_consumed="
                  << pump_counters.do_work_consumed
                  << " pump_wake_dispatches="
                  << pump_counters.wake_dispatches
                  << " tasks_posted=" << task_counters.posted_tasks
                  << " tasks_started=" << task_counters.started_tasks
                  << " tasks_completed=" << task_counters.completed_tasks;
  }
  EXPECT_GE(counters.open_reached, 1u);
  if (counters.open_reached > 0) {
    EXPECT_GE(counters.write_reached, 1u);
  }
  if (counters.write_reached > 0) {
    EXPECT_GE(counters.read_reached, 1u);
  }
  EXPECT_TRUE(ok.load(std::memory_order_acquire));
  EXPECT_TRUE(callback_on_io_thread.load(std::memory_order_acquire));
  EXPECT_FALSE(callback_inline_violation.load(std::memory_order_acquire));
  EXPECT_GT(pump_counters.do_work_calls, 0u);
  EXPECT_GT(task_counters.started_tasks, 0u);
  EXPECT_GT(task_counters.completed_tasks, 0u);

  background_thread.Stop();
  io_thread.Stop();
  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

TEST(AsyncFileWinTest, RepeatedStressMaintainsCallbackThreadDeterminism) {
  AsyncFileWin::ResetStageCountersForTesting();
  MessagePumpForIO::ResetDebugCountersForTesting();
  TaskRunner::ResetTracingStatsForTesting();

  Thread io_thread("async-file-io-stress");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread background_thread("async-file-bg-stress");
  ASSERT_TRUE(background_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> bg_runner = background_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(bg_runner);

  const std::filesystem::path path = MakeTempFilePath("stress_det");
  const std::string path_utf8 = PathToUTF8(path);
  constexpr int kRounds = 40;
  const std::size_t kBytesPerRound = 2u * 1024u * 1024u;

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{true};
  std::atomic<bool> callback_on_io_thread{true};
  std::atomic<bool> callback_inline_violation{false};
  std::atomic<PlatformThread::PlatformThreadId> io_tid{0};

  io_runner->PostTask(FROM_HERE, [&, io_runner, bg_runner, path_utf8]() mutable {
    io_tid.store(PlatformThread::CurrentId(), std::memory_order_release);
    auto file = std::make_shared<AsyncFileWin>(io_runner);

    file->OpenAsync(
        path_utf8, AsyncFile::OpenMode::kReadWrite,
        AsyncFile::OpenDisposition::kCreateAlways, bg_runner,
        [&, file](bool open_success, AsyncFile::Error open_error) mutable {
          if (PlatformThread::CurrentId() !=
              io_tid.load(std::memory_order_acquire)) {
            callback_on_io_thread.store(false, std::memory_order_release);
          }
          if (!open_success || !open_error.ok()) {
            ok.store(false, std::memory_order_release);
            file->CloseAsync([&]() { done.Signal(); });
            return;
          }

          auto run_round = std::make_shared<std::function<void(int)>>();
          *run_round = [&, file, run_round](int round) mutable {
            if (round >= kRounds) {
              file->CloseAsync([&]() { done.Signal(); });
              return;
            }

            const std::vector<std::uint8_t> expected =
                MakePayload(kBytesPerRound, static_cast<std::uint8_t>(round));

            auto write_called = std::make_shared<std::atomic<bool>>(false);
            const bool write_accepted = IssueWrite(
              file,
                0, expected,
                [&, file, run_round, round, expected,
                 write_called](bool write_success, std::size_t bytes_written,
                               AsyncFile::Error write_error) mutable {
                  write_called->store(true, std::memory_order_release);
                  if (PlatformThread::CurrentId() !=
                      io_tid.load(std::memory_order_acquire)) {
                    callback_on_io_thread.store(false,
                                                std::memory_order_release);
                  }
                  if (!write_success || !write_error.ok() ||
                      bytes_written != expected.size()) {
                    ok.store(false, std::memory_order_release);
                    file->CloseAsync([&]() { done.Signal(); });
                    return;
                  }

                  auto read_called = std::make_shared<std::atomic<bool>>(false);
                    const bool read_accepted = IssueRead(
                      file,
                      0, expected.size(),
                      [&, file, run_round, round, expected,
                       read_called](bool read_success,
                                    std::vector<std::uint8_t>&& data,
                                    AsyncFile::Error read_error) mutable {
                        read_called->store(true, std::memory_order_release);
                        if (PlatformThread::CurrentId() !=
                            io_tid.load(std::memory_order_acquire)) {
                          callback_on_io_thread.store(false,
                                                      std::memory_order_release);
                        }
                        if (!read_success || !read_error.ok() ||
                            data != expected) {
                          ok.store(false, std::memory_order_release);
                          file->CloseAsync([&]() { done.Signal(); });
                          return;
                        }
                        (*run_round)(round + 1);
                      });

                  if (!read_accepted) {
                    ok.store(false, std::memory_order_release);
                    file->CloseAsync([&]() { done.Signal(); });
                    return;
                  }
                  if (read_called->load(std::memory_order_acquire)) {
                    callback_inline_violation.store(true,
                                                   std::memory_order_release);
                  }
                });

            if (!write_accepted) {
              ok.store(false, std::memory_order_release);
              file->CloseAsync([&]() { done.Signal(); });
              return;
            }
            if (write_called->load(std::memory_order_acquire)) {
              callback_inline_violation.store(true,
                                             std::memory_order_release);
            }
          };

          (*run_round)(0);
        });
  });

  const bool completed = done.TimedWait(std::chrono::seconds(60));
  EXPECT_TRUE(completed);
  const AsyncFileWin::StageCounters counters =
      AsyncFileWin::GetStageCountersForTesting();
    const MessagePumpForIO::DebugCounters pump_counters =
      MessagePumpForIO::GetDebugCountersForTesting();
    const TaskRunnerTracingStats task_counters =
      TaskRunner::GetTracingStatsForTesting();
  if (!completed) {
    ADD_FAILURE() << "stage counters: open=" << counters.open_reached
                  << " write=" << counters.write_reached
                  << " read=" << counters.read_reached
                  << " iocp_completed=" << counters.iocp_completed
                  << " context_hit=" << counters.context_hit
                  << " context_miss=" << counters.context_miss
                  << " read_finalize_attempted="
                  << counters.read_finalize_attempted
                  << " read_posted=" << counters.read_posted
                  << " callback_weak_dropped="
                  << counters.callback_weak_dropped
                  << " callback_post_failed="
                  << counters.callback_post_failed
                  << " write_post_seq=" << counters.write_post_seq
                  << " write_exec_seq=" << counters.write_exec_seq
                  << " read_post_seq=" << counters.read_post_seq
                  << " read_exec_seq=" << counters.read_exec_seq
                  << " pump_do_work_calls="
                  << pump_counters.do_work_calls
                  << " pump_do_work_consumed="
                  << pump_counters.do_work_consumed
                  << " pump_wake_dispatches="
                  << pump_counters.wake_dispatches
                  << " tasks_posted=" << task_counters.posted_tasks
                  << " tasks_started=" << task_counters.started_tasks
                  << " tasks_completed=" << task_counters.completed_tasks;
  }
  EXPECT_GE(counters.open_reached, 1u);
  if (counters.open_reached > 0) {
    EXPECT_GE(counters.write_reached, 1u);
  }
  if (counters.write_reached > 0) {
    EXPECT_GE(counters.read_reached, 1u);
  }
  EXPECT_TRUE(ok.load(std::memory_order_acquire));
  EXPECT_TRUE(callback_on_io_thread.load(std::memory_order_acquire));
  EXPECT_FALSE(callback_inline_violation.load(std::memory_order_acquire));
  EXPECT_GT(pump_counters.do_work_calls, 0u);
  EXPECT_GT(task_counters.started_tasks, 0u);
  EXPECT_GT(task_counters.completed_tasks, 0u);

  background_thread.Stop();
  io_thread.Stop();
  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

TEST(AsyncFileWinTest, AppendModeAppendsIgnoringCallerOffsetInOrder) {
  Thread io_thread("async-file-append-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread background_thread("async-file-append-bg");
  ASSERT_TRUE(background_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> bg_runner = background_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(bg_runner);

  const std::filesystem::path path = MakeTempFilePath("append_order");
  const std::string path_utf8 = PathToUTF8(path);
  const std::vector<std::uint8_t> payload1 = MakePayload(64 * 1024, 11);
  const std::vector<std::uint8_t> payload2 = MakePayload(64 * 1024, 23);

  auto file = std::make_shared<AsyncFileWin>(io_runner);
  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write1_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write2_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> open_ok{false};
  std::atomic<bool> write1_ok{false};
  std::atomic<bool> write2_ok{false};
  std::atomic<std::size_t> write1_bytes{0};
  std::atomic<std::size_t> write2_bytes{0};

  file->OpenAsync(path_utf8, AsyncFile::OpenMode::kAppend,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner,
                  [&](bool success, AsyncFile::Error error) {
                    open_ok.store(success && error.ok(),
                                  std::memory_order_release);
                    open_done.Signal();
                  });

  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

    const bool write1_accepted = IssueWrite(
      file,
      0, payload1,
      [&](bool success, std::size_t bytes_written, AsyncFile::Error error) {
        write1_ok.store(success && error.ok(),
                        std::memory_order_release);
        write1_bytes.store(bytes_written, std::memory_order_release);
        write1_done.Signal();
      });
  ASSERT_TRUE(write1_accepted);
  ASSERT_TRUE(write1_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write1_ok.load(std::memory_order_acquire));
  ASSERT_EQ(write1_bytes.load(std::memory_order_acquire), payload1.size());

    const bool write2_accepted = IssueWrite(
      file,
      0, payload2,
      [&](bool success, std::size_t bytes_written, AsyncFile::Error error) {
        write2_ok.store(success && error.ok(),
                        std::memory_order_release);
        write2_bytes.store(bytes_written, std::memory_order_release);
        write2_done.Signal();
      });
  ASSERT_TRUE(write2_accepted);
  ASSERT_TRUE(write2_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write2_ok.load(std::memory_order_acquire));
  ASSERT_EQ(write2_bytes.load(std::memory_order_acquire), payload2.size());

  file->CloseAsync([&]() { close_barrier.Signal(); });  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));

  auto reader = std::make_shared<AsyncFileWin>(io_runner);
  WaitableEvent read_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> read_open_ok{false};
  std::atomic<bool> read_ok{false};
  std::vector<std::uint8_t> all_data;

  reader->OpenAsync(path_utf8, AsyncFile::OpenMode::kReadWrite,
                    AsyncFile::OpenDisposition::kOpenExisting, bg_runner,
                    [&](bool success, AsyncFile::Error error) {
                      read_open_ok.store(success && error.ok(),
                                         std::memory_order_release);
                      read_open_done.Signal();
                    });
  ASSERT_TRUE(read_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_open_ok.load(std::memory_order_acquire));

  const std::size_t total_size = payload1.size() + payload2.size();
    const bool read_accepted = IssueRead(
      reader,
      0, total_size,
      [&](bool success, std::vector<std::uint8_t>&& data, AsyncFile::Error error) {
        read_ok.store(success && error.ok(),
                      std::memory_order_release);
        all_data = std::move(data);
        read_done.Signal();
      });
  ASSERT_TRUE(read_accepted);
  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(all_data.size(), total_size);

  ASSERT_TRUE(std::equal(payload1.begin(), payload1.end(), all_data.begin()));
  ASSERT_TRUE(std::equal(payload2.begin(), payload2.end(),
                         all_data.begin() + static_cast<std::ptrdiff_t>(payload1.size())));

  reader->CloseAsync([&]() { read_close_barrier.Signal(); });  ASSERT_TRUE(read_close_barrier.TimedWait(std::chrono::seconds(10)));

  background_thread.Stop();
  io_thread.Stop();
  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

TEST(AsyncFileWinTest, ConcurrentAppendPreservesWholeWriteBlocks) {
  Thread io_thread("async-file-append-concurrent-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread background_thread("async-file-append-concurrent-bg");
  ASSERT_TRUE(background_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> bg_runner = background_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(bg_runner);

  constexpr int kWriters = 8;
  constexpr int kRoundsPerWriter = 8;
  constexpr std::size_t kBlockSize = 4096;

  const std::filesystem::path path = MakeTempFilePath("append_concurrent");
  const std::string path_utf8 = PathToUTF8(path);

  std::array<std::vector<std::uint8_t>, kWriters> payloads;
  for (int i = 0; i < kWriters; ++i) {
    payloads[static_cast<std::size_t>(i)].assign(
        kBlockSize, static_cast<std::uint8_t>('A' + i));
  }

  auto file = std::make_shared<AsyncFileWin>(io_runner);
  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent writes_done(WaitableEvent::ResetPolicy::kManual, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> open_ok{false};
  std::atomic<bool> callbacks_ok{true};
  std::atomic<int> remaining_callbacks{kWriters * kRoundsPerWriter};

  file->OpenAsync(path_utf8, AsyncFile::OpenMode::kAppend,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner,
                  [&](bool success, AsyncFile::Error error) {
                    open_ok.store(success && error.ok(),
                                  std::memory_order_release);
                    open_done.Signal();
                  });
  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w]() {
      for (int r = 0; r < kRoundsPerWriter; ++r) {
        const bool accepted = IssueWrite(
          file,
            0, payloads[static_cast<std::size_t>(w)],
            [&](bool success, std::size_t bytes_written, AsyncFile::Error error) {
              if (!success || !error.ok() || bytes_written != kBlockSize) {
                callbacks_ok.store(false, std::memory_order_release);
              }
              if (remaining_callbacks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                writes_done.Signal();
              }
            });
        if (!accepted) {
          callbacks_ok.store(false, std::memory_order_release);
          if (remaining_callbacks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            writes_done.Signal();
          }
        }
      }
    });
  }

  for (auto& t : writers) {
    t.join();
  }

  ASSERT_TRUE(writes_done.TimedWait(std::chrono::seconds(20)));
  ASSERT_TRUE(callbacks_ok.load(std::memory_order_acquire));

  file->CloseAsync([&]() { close_barrier.Signal(); });  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));

  auto reader = std::make_shared<AsyncFileWin>(io_runner);
  WaitableEvent read_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> read_open_ok{false};
  std::atomic<bool> read_ok{false};
  std::vector<std::uint8_t> all_data;

  reader->OpenAsync(path_utf8, AsyncFile::OpenMode::kReadWrite,
                    AsyncFile::OpenDisposition::kOpenExisting, bg_runner,
                    [&](bool success, AsyncFile::Error error) {
                      read_open_ok.store(success && error.ok(),
                                         std::memory_order_release);
                      read_open_done.Signal();
                    });
  ASSERT_TRUE(read_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_open_ok.load(std::memory_order_acquire));

  const std::size_t expected_total =
      static_cast<std::size_t>(kWriters * kRoundsPerWriter) * kBlockSize;
    const bool read_accepted = IssueRead(
      reader,
      0, expected_total,
      [&](bool success, std::vector<std::uint8_t>&& data, AsyncFile::Error error) {
        read_ok.store(success && error.ok(),
                      std::memory_order_release);
        all_data = std::move(data);
        read_done.Signal();
      });
  ASSERT_TRUE(read_accepted);
  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(20)));
  ASSERT_TRUE(read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(all_data.size(), expected_total);

  std::array<int, kWriters> block_counts{};
  for (std::size_t pos = 0; pos < all_data.size(); pos += kBlockSize) {
    const std::uint8_t token = all_data[pos];
    for (std::size_t j = 1; j < kBlockSize; ++j) {
      ASSERT_EQ(all_data[pos + j], token);
    }
    const int idx = static_cast<int>(token) - static_cast<int>('A');
    ASSERT_GE(idx, 0);
    ASSERT_LT(idx, kWriters);
    ++block_counts[static_cast<std::size_t>(idx)];
  }

  for (int i = 0; i < kWriters; ++i) {
    EXPECT_EQ(block_counts[static_cast<std::size_t>(i)], kRoundsPerWriter);
  }

  reader->CloseAsync([&]() { read_close_barrier.Signal(); });  ASSERT_TRUE(read_close_barrier.TimedWait(std::chrono::seconds(10)));

  background_thread.Stop();
  io_thread.Stop();
  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

TEST(AsyncFileWinTest, FileReadParsesLinesWithAsyncLineReader) {
  Thread io_thread("async-file-line-reader-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread background_thread("async-file-line-reader-bg");
  ASSERT_TRUE(background_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> bg_runner = background_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(bg_runner);

  const std::filesystem::path path = MakeTempFilePath("line_reader");
  const std::string path_utf8 = PathToUTF8(path);
  const std::string text = "alpha\r\nbeta\ngamma\r\nlast_line";
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());

  auto writer = std::make_shared<AsyncFileWin>(io_runner);
  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write_close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> open_ok{false};
  std::atomic<bool> write_ok{false};

  writer->OpenAsync(path_utf8, AsyncFile::OpenMode::kReadWrite,
                    AsyncFile::OpenDisposition::kCreateAlways, bg_runner,
                    [&](bool success, AsyncFile::Error error) {
                      open_ok.store(success && error.ok(),
                                    std::memory_order_release);
                      open_done.Signal();
                    });
  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

    const bool write_accepted = IssueWrite(
      writer,
      0, bytes,
      [&](bool success, std::size_t wrote, AsyncFile::Error error) {
        write_ok.store(success && error.ok() && wrote == bytes.size(),
                       std::memory_order_release);
        write_done.Signal();
      });
  ASSERT_TRUE(write_accepted);
  ASSERT_TRUE(write_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write_ok.load(std::memory_order_acquire));

  writer->CloseAsync([&]() { write_close_barrier.Signal(); });  ASSERT_TRUE(write_close_barrier.TimedWait(std::chrono::seconds(10)));

  auto reader_file = std::make_shared<AsyncFileWin>(io_runner);
  WaitableEvent read_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> read_open_ok{false};

  reader_file->OpenAsync(path_utf8, AsyncFile::OpenMode::kReadOnly,
                         AsyncFile::OpenDisposition::kOpenExisting, bg_runner,
                         [&](bool success, AsyncFile::Error error) {
                           read_open_ok.store(success && error.ok(),
                                              std::memory_order_release);
                           read_open_done.Signal();
                         });
  ASSERT_TRUE(read_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_open_ok.load(std::memory_order_acquire));

  FileBackedAsyncInputStream input_stream(reader_file, 5);
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
  reader_file->CloseAsync([&]() { read_close_barrier.Signal(); });  ASSERT_TRUE(read_close_barrier.TimedWait(std::chrono::seconds(10)));

  background_thread.Stop();
  io_thread.Stop();
  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

TEST(AsyncFileWinTest, RaiiDestructionDoesNotHangAndDataIsFlushed) {
  Thread io_thread("async-file-raii-io");
  Thread::Options io_options;
  io_options.message_pump_type = MessagePumpType::IO;
  ASSERT_TRUE(io_thread.StartWithOptions(io_options));

  Thread background_thread("async-file-raii-bg");
  ASSERT_TRUE(background_thread.Start());

  scoped_refptr<TaskRunner> io_runner = io_thread.GetTaskRunner();
  scoped_refptr<TaskRunner> bg_runner = background_thread.GetTaskRunner();
  ASSERT_TRUE(io_runner);
  ASSERT_TRUE(bg_runner);

  const std::filesystem::path path = MakeTempFilePath("raii_destruct");
  const std::string path_utf8 = PathToUTF8(path);
  const std::vector<std::uint8_t> payload = MakePayload(4096, 0xAB);

  // Scope block: create, open, write, then release  --  RAII destruction.
  {
    auto file = std::make_shared<AsyncFileWin>(io_runner);
    ASSERT_TRUE(file);

    WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
    std::atomic<bool> open_ok{false};
    file->OpenAsync(path_utf8, AsyncFile::OpenMode::kReadWrite,
                    AsyncFile::OpenDisposition::kCreateAlways, bg_runner,
                    [&](bool success, AsyncFile::Error error) {
                      open_ok.store(success && error.ok(),
                                    std::memory_order_release);
                      open_done.Signal();
                    });
    ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
    ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

    WaitableEvent write_done(WaitableEvent::ResetPolicy::kAutomatic, false);
    std::atomic<bool> write_ok{false};
    auto buf = MakeRefCounted<IOBufferWithSize>(payload.size());
    std::memcpy(buf->data(), payload.data(), payload.size());
    const bool accepted = IssueWrite(
        file, 0, payload,
        [&](bool success, std::size_t wrote, AsyncFile::Error error) {
          write_ok.store(success && error.ok() && wrote == payload.size(),
                         std::memory_order_release);
          write_done.Signal();
        });
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(write_done.TimedWait(std::chrono::seconds(10)));
    ASSERT_TRUE(write_ok.load(std::memory_order_acquire));

    // file.reset() happens here  --  must not hang or crash.
  }

  // Let the IO thread process the orphan's CancelAndSelfDestruct task.
  WaitableEvent settle(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner->PostTask(FROM_HERE, [&]() { settle.Signal(); });
  ASSERT_TRUE(settle.TimedWait(std::chrono::seconds(10)));

  // Verify data was persisted.
  auto reader = std::make_shared<AsyncFileWin>(io_runner);
  WaitableEvent read_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_close_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> read_open_ok{false};
  std::atomic<bool> read_ok{false};

  reader->OpenAsync(path_utf8, AsyncFile::OpenMode::kReadOnly,
                    AsyncFile::OpenDisposition::kOpenExisting, bg_runner,
                    [&](bool success, AsyncFile::Error error) {
                      read_open_ok.store(success && error.ok(),
                                         std::memory_order_release);
                      read_open_done.Signal();
                    });
  ASSERT_TRUE(read_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_open_ok.load(std::memory_order_acquire));

  auto rbuf = scoped_refptr<IOBuffer>(new IOBufferWithSize(payload.size()));
  reader->ReadAsync(rbuf, payload.size(), 0,
                    [&](bool success, std::size_t n, AsyncFile::Error error) {
                      read_ok.store(success && error.ok() && n == payload.size(),
                                    std::memory_order_release);
                      read_done.Signal();
                    });
  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_ok.load(std::memory_order_acquire));

  std::vector<std::uint8_t> read_back(payload.size());
  std::memcpy(read_back.data(), rbuf->data(), payload.size());
  EXPECT_EQ(read_back, payload);

  reader->CloseAsync([&]() { read_close_done.Signal(); });
  ASSERT_TRUE(read_close_done.TimedWait(std::chrono::seconds(10)));

  background_thread.Stop();
  io_thread.Stop();
  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

}  // namespace
}  // namespace nei

#endif  // defined(_WIN32)
