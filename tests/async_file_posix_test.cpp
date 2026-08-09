#include <gtest/gtest.h>

#if !defined(_WIN32)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <cerrno>

#include <neixx/common/location.h>
#include <async_file_posix.h>
#include <neixx/io/io_buffer.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread.h>
#if __cplusplus >= 202002L
namespace {
inline std::string PathToUTF8(const std::filesystem::path &p) {
  auto u = p.u8string();
  return {reinterpret_cast<const char *>(u.data()), u.size()};
}
} // namespace
#else
namespace {
inline std::string PathToUTF8(const std::filesystem::path &p) {
  return p.u8string();
}
} // namespace
#endif

namespace nei {
namespace {

std::filesystem::path MakeTempFilePath(const char *name_hint) {
  const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::filesystem::path p = std::filesystem::temp_directory_path();
  p /= std::string("nei_async_file_posix_") + name_hint + "_" + std::to_string(static_cast<long long>(ticks)) + ".bin";
  return p;
}

std::vector<std::uint8_t> MakePayload(std::size_t size, std::uint8_t salt) {
  std::vector<std::uint8_t> data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<std::uint8_t>((i * 131u + salt) & 0xFFu);
  }
  return data;
}

bool IssueWrite(const std::shared_ptr<AsyncFilePosix> &file,
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

bool IssueRead(const std::shared_ptr<AsyncFilePosix> &file,
               std::uint64_t offset,
               std::size_t bytes_to_read,
               std::function<void(bool success, std::vector<std::uint8_t> &&data, AsyncFile::Error error)> callback) {
  if (!file || !callback) {
    return false;
  }
  scoped_refptr<IOBuffer> buf(new IOBufferWithSize(bytes_to_read));
  file->ReadAsync(
      buf,
      bytes_to_read,
      offset,
      [buf, callback = std::move(callback)](bool success, std::size_t bytes_read, AsyncFile::Error error) mutable {
        std::vector<std::uint8_t> out;
        if (bytes_read > 0) {
          out.resize(bytes_read);
          std::memcpy(out.data(), buf->data(), bytes_read);
        }
        callback(success, std::move(out), error);
      });
  return true;
}

class AsyncFilePosixTest : public testing::Test {
protected:
  void SetUp() override {
    Thread::Options io_options;
    io_options.message_pump_type = MessagePumpType::IO;
    ASSERT_TRUE(io_thread_.StartWithOptions(io_options));
    ASSERT_TRUE(background_thread_.Start());

    io_runner_ = io_thread_.GetTaskRunner();
    bg_runner_ = background_thread_.GetTaskRunner();
    ASSERT_TRUE(io_runner_);
    ASSERT_TRUE(bg_runner_);
  }

  void TearDown() override {
    background_thread_.Stop();
    io_thread_.Stop();

    for (const auto &path : cleanup_files_) {
      std::error_code ec;
      (void)std::filesystem::remove(path, ec);
    }
  }

  std::filesystem::path NewTempPath(const char *hint) {
    std::filesystem::path path = MakeTempFilePath(hint);
    cleanup_files_.push_back(path);
    return path;
  }

  Thread io_thread_{"async-file-posix-io"};
  Thread background_thread_{"async-file-posix-bg"};
  scoped_refptr<SingleThreadTaskRunner> io_runner_;
  scoped_refptr<SingleThreadTaskRunner> bg_runner_;
  std::vector<std::filesystem::path> cleanup_files_;
};

class AsyncFilePosixStressTest : public AsyncFilePosixTest {};

TEST_F(AsyncFilePosixTest, OpenFailureRollsBackAndAllowsRetry) {
  const std::filesystem::path missing_path = NewTempPath("open_retry_missing");
  std::error_code ec;
  (void)std::filesystem::remove(missing_path, ec);

  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  // Heap-allocated state captured BY VALUE in async callbacks.  Async
  // callbacks may be delivered after the test body's stack frames unwind
  // (e.g. a chunk completion racing with CloseAsync), so capturing stack
  // locals by reference would be a use-after-free (TSan-confirmed:
  // cond_destroy vs cond_signal on close_barrier).  A shared_ptr keeps the
  // state alive for as long as any in-flight operation references it.
  struct State {
    WaitableEvent first_open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent second_open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent close_barrier{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> first_ok{true};
    std::atomic<bool> second_ok{false};
    AsyncFile::ErrorCode first_error = AsyncFile::ErrorCode::kOk;
  };

  auto state = std::make_shared<State>();

  file->OpenAsync(missing_path.string(),
                  AsyncFile::OpenMode::kReadOnly,
                  AsyncFile::OpenDisposition::kOpenExisting,
                  bg_runner_,
                  [state](bool success, AsyncFile::Error error) {
                    state->first_ok.store(!success, std::memory_order_release);
                    state->first_error = error.code;
                    state->first_open_done.Signal();
                  });

  ASSERT_TRUE(state->first_open_done.TimedWait(std::chrono::seconds(10)));
  EXPECT_TRUE(state->first_ok.load(std::memory_order_acquire));
  EXPECT_EQ(state->first_error, AsyncFile::ErrorCode::kNotFound);
  EXPECT_FALSE(file->is_open());

  file->OpenAsync(missing_path.string(),
                  AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways,
                  bg_runner_,
                  [state](bool success, AsyncFile::Error error) {
                    state->second_ok.store(success && error.ok(), std::memory_order_release);
                    state->second_open_done.Signal();
                  });

  ASSERT_TRUE(state->second_open_done.TimedWait(std::chrono::seconds(10)));
  EXPECT_TRUE(state->second_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(file->is_open());

  file->CloseAsync([state]() { state->close_barrier.Signal(); });
  ASSERT_TRUE(state->close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixTest, PositionalConcurrentWritesAndReadbackAreStable) {
  const std::filesystem::path path = NewTempPath("positional_rw");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  // Heap-allocated state captured BY VALUE in async callbacks.  Async
  // callbacks may be delivered after the test body's stack frames unwind
  // (e.g. a chunk completion racing with CloseAsync), so capturing stack
  // locals by reference would be a use-after-free.  A shared_ptr keeps the
  // state alive for as long as any in-flight operation references it.
  struct State {
    WaitableEvent open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent write_done{WaitableEvent::ResetPolicy::kManual, false};
    WaitableEvent read_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent close_barrier{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> open_ok{false};
    std::atomic<bool> write_ok{true};
    std::atomic<int> pending_writes{2};
    std::atomic<bool> read_ok{false};
    std::atomic<std::size_t> first_bad_write_bytes{0};
    std::atomic<std::uint32_t> first_bad_write_native{0};
    std::atomic<int> first_bad_write_code{static_cast<int>(AsyncFile::ErrorCode::kOk)};
    std::vector<std::uint8_t> read_back;
  };

  auto state = std::make_shared<State>();

  const std::vector<std::uint8_t> payload_a = MakePayload(64 * 1024, 11);
  const std::vector<std::uint8_t> payload_b = MakePayload(64 * 1024, 29);
  const std::int64_t offset_a = 0;
  const std::int64_t offset_b = 256 * 1024;

  file->OpenAsync(path.string(),
                  AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways,
                  bg_runner_,
                  [state](bool success, AsyncFile::Error error) {
                    state->open_ok.store(success && error.ok(), std::memory_order_release);
                    state->open_done.Signal();
                  });
  ASSERT_TRUE(state->open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->open_ok.load(std::memory_order_acquire));

  auto launch_write = [state, &file](std::int64_t offset, std::vector<std::uint8_t> payload) {
    const std::size_t expect_bytes = payload.size();
    const bool accepted =
        IssueWrite(file,
                   offset,
                   std::move(payload),
                   [state, expect_bytes](bool success, std::size_t bytes_written, AsyncFile::Error error) {
                     if (!success || !error.ok() || bytes_written != expect_bytes) {
                       state->first_bad_write_bytes.store(bytes_written, std::memory_order_release);
                       state->first_bad_write_native.store(error.native_code, std::memory_order_release);
                       state->first_bad_write_code.store(static_cast<int>(error.code), std::memory_order_release);
                       state->write_ok.store(false, std::memory_order_release);
                     }
                     if (state->pending_writes.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                       state->write_done.Signal();
                     }
                   });

    if (!accepted) {
      state->write_ok.store(false, std::memory_order_release);
      if (state->pending_writes.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        state->write_done.Signal();
      }
    }
  };

  std::thread t1([&]() { launch_write(offset_a, payload_a); });
  std::thread t2([&]() { launch_write(offset_b, payload_b); });
  t1.join();
  t2.join();

  ASSERT_TRUE(state->write_done.TimedWait(std::chrono::seconds(15)));
  ASSERT_TRUE(state->write_ok.load(std::memory_order_acquire))
      << "bad write callback: bytes=" << state->first_bad_write_bytes.load(std::memory_order_acquire)
      << " code=" << state->first_bad_write_code.load(std::memory_order_acquire)
      << " native=" << state->first_bad_write_native.load(std::memory_order_acquire);

  const std::size_t read_size = static_cast<std::size_t>(offset_b) + payload_b.size();
  const bool read_accepted =
      IssueRead(file, 0, read_size, [state](bool success, std::vector<std::uint8_t> &&data, AsyncFile::Error error) {
        state->read_ok.store(success && error.ok(), std::memory_order_release);
        state->read_back = std::move(data);
        state->read_done.Signal();
      });
  ASSERT_TRUE(read_accepted);
  ASSERT_TRUE(state->read_done.TimedWait(std::chrono::seconds(15)));
  ASSERT_TRUE(state->read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(state->read_back.size(), read_size);

  ASSERT_TRUE(std::equal(payload_a.begin(), payload_a.end(), state->read_back.begin()));
  for (std::size_t i = payload_a.size(); i < static_cast<std::size_t>(offset_b); ++i) {
    ASSERT_EQ(state->read_back[i], 0u);
  }
  ASSERT_TRUE(
      std::equal(payload_b.begin(), payload_b.end(), state->read_back.begin() + static_cast<std::ptrdiff_t>(offset_b)));

  file->CloseAsync([state]() { state->close_barrier.Signal(); });
  ASSERT_TRUE(state->close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixTest, AppendModeAppendsIgnoringCallerOffset) {
  const std::filesystem::path path = NewTempPath("append");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  // Heap-allocated state captured BY VALUE in async callbacks so late
  // completions never signal a destroyed WaitableEvent (TSan-confirmed).
  struct State {
    WaitableEvent open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent write1_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent write2_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent read_open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent read_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent close_barrier{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> open_ok{false};
    std::atomic<bool> write1_ok{false};
    std::atomic<bool> write2_ok{false};
    std::atomic<bool> read_open_ok{false};
    std::atomic<bool> read_ok{false};
    std::vector<std::uint8_t> all_data;
  };

  auto state = std::make_shared<State>();

  const std::vector<std::uint8_t> payload1 = MakePayload(32 * 1024, 3);
  const std::vector<std::uint8_t> payload2 = MakePayload(32 * 1024, 7);

  file->OpenAsync(path.string(),
                  AsyncFile::OpenMode::kAppend,
                  AsyncFile::OpenDisposition::kCreateAlways,
                  bg_runner_,
                  [state](bool success, AsyncFile::Error error) {
                    state->open_ok.store(success && error.ok(), std::memory_order_release);
                    state->open_done.Signal();
                  });
  ASSERT_TRUE(state->open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->open_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(
      IssueWrite(file, 0, payload1, [state, payload1](bool success, std::size_t bytes_written, AsyncFile::Error error) {
        state->write1_ok.store(success && error.ok() && bytes_written == payload1.size(), std::memory_order_release);
        state->write1_done.Signal();
      }));
  ASSERT_TRUE(state->write1_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->write1_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(
      IssueWrite(file, 0, payload2, [state, payload2](bool success, std::size_t bytes_written, AsyncFile::Error error) {
        state->write2_ok.store(success && error.ok() && bytes_written == payload2.size(), std::memory_order_release);
        state->write2_done.Signal();
      }));
  ASSERT_TRUE(state->write2_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->write2_ok.load(std::memory_order_acquire));

  file->CloseAsync([state]() { state->close_barrier.Signal(); });
  ASSERT_TRUE(state->close_barrier.TimedWait(std::chrono::seconds(10)));

  auto reader = std::make_shared<AsyncFilePosix>(io_runner_);
  reader->OpenAsync(path.string(),
                    AsyncFile::OpenMode::kReadOnly,
                    AsyncFile::OpenDisposition::kOpenExisting,
                    bg_runner_,
                    [state](bool success, AsyncFile::Error error) {
                      state->read_open_ok.store(success && error.ok(), std::memory_order_release);
                      state->read_open_done.Signal();
                    });
  ASSERT_TRUE(state->read_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->read_open_ok.load(std::memory_order_acquire));

  const std::size_t total_size = payload1.size() + payload2.size();
  ASSERT_TRUE(
      IssueRead(reader, 0, total_size, [state](bool success, std::vector<std::uint8_t> &&data, AsyncFile::Error error) {
        state->read_ok.store(success && error.ok(), std::memory_order_release);
        state->all_data = std::move(data);
        state->read_done.Signal();
      }));
  ASSERT_TRUE(state->read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(state->all_data.size(), total_size);
  ASSERT_TRUE(std::equal(payload1.begin(), payload1.end(), state->all_data.begin()));
  ASSERT_TRUE(std::equal(
      payload2.begin(), payload2.end(), state->all_data.begin() + static_cast<std::ptrdiff_t>(payload1.size())));
}

TEST_F(AsyncFilePosixTest, ReadPastEndReturnsPartialDataAsSuccess) {
  const std::filesystem::path path = NewTempPath("eof_partial");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  // Heap-allocated state captured BY VALUE in async callbacks so late
  // completions never signal a destroyed WaitableEvent (TSan-confirmed).
  struct State {
    WaitableEvent open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent write_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent read_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent close_barrier{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> open_ok{false};
    std::atomic<bool> write_ok{false};
    std::atomic<bool> read_ok{false};
    std::vector<std::uint8_t> out;
  };

  auto state = std::make_shared<State>();

  const std::vector<std::uint8_t> payload = MakePayload(4096, 91);

  file->OpenAsync(path.string(),
                  AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways,
                  bg_runner_,
                  [state](bool success, AsyncFile::Error error) {
                    state->open_ok.store(success && error.ok(), std::memory_order_release);
                    state->open_done.Signal();
                  });
  ASSERT_TRUE(state->open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->open_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(
      IssueWrite(file, 0, payload, [state, payload](bool success, std::size_t bytes_written, AsyncFile::Error error) {
        state->write_ok.store(success && error.ok() && bytes_written == payload.size(), std::memory_order_release);
        state->write_done.Signal();
      }));
  ASSERT_TRUE(state->write_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->write_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(IssueRead(
      file, 0, payload.size() * 4, [state](bool success, std::vector<std::uint8_t> &&data, AsyncFile::Error error) {
        state->read_ok.store(success && error.ok(), std::memory_order_release);
        state->out = std::move(data);
        state->read_done.Signal();
      }));
  ASSERT_TRUE(state->read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(state->out, payload);

  file->CloseAsync([state]() { state->close_barrier.Signal(); });
  ASSERT_TRUE(state->close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixTest, LargeOffsetRandomAccessBeyond2GBWorks) {
  const std::filesystem::path path = NewTempPath("large_offset");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  // Heap-allocated state captured BY VALUE in async callbacks so late
  // completions never signal a destroyed WaitableEvent (TSan-confirmed).
  struct State {
    WaitableEvent open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent write_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent read_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent close_barrier{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> open_ok{false};
    std::atomic<bool> write_ok{false};
    std::atomic<bool> read_ok{false};
    std::vector<std::uint8_t> out;
  };

  auto state = std::make_shared<State>();

  const std::int64_t large_offset = static_cast<std::int64_t>(3LL * 1024 * 1024 * 1024 + 123);
  const std::vector<std::uint8_t> payload = MakePayload(256, 55);

  file->OpenAsync(path.string(),
                  AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways,
                  bg_runner_,
                  [state](bool success, AsyncFile::Error error) {
                    state->open_ok.store(success && error.ok(), std::memory_order_release);
                    state->open_done.Signal();
                  });
  ASSERT_TRUE(state->open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->open_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(IssueWrite(
      file, large_offset, payload, [state, payload](bool success, std::size_t bytes_written, AsyncFile::Error error) {
        state->write_ok.store(success && error.ok() && bytes_written == payload.size(), std::memory_order_release);
        state->write_done.Signal();
      }));
  ASSERT_TRUE(state->write_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->write_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(IssueRead(file,
                        large_offset,
                        payload.size(),
                        [state](bool success, std::vector<std::uint8_t> &&data, AsyncFile::Error error) {
                          state->read_ok.store(success && error.ok(), std::memory_order_release);
                          state->out = std::move(data);
                          state->read_done.Signal();
                        }));
  ASSERT_TRUE(state->read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(state->out, payload);

  file->CloseAsync([state]() { state->close_barrier.Signal(); });
  ASSERT_TRUE(state->close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixStressTest, HighConcurrencyMixedReadWriteChain) {
  const std::filesystem::path path = NewTempPath("stress_mixed_rw");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  // Heap-allocated state captured BY VALUE in async callbacks so late
  // completions never signal a destroyed WaitableEvent (TSan-confirmed).
  struct State {
    WaitableEvent open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent all_done{WaitableEvent::ResetPolicy::kManual, false};
    WaitableEvent close_barrier{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> open_ok{false};
    std::atomic<int> remaining_reads{0};
  };

  auto state = std::make_shared<State>();

  constexpr int kProducerThreads = 4;
  constexpr int kOpsPerProducer = 10;
  constexpr std::size_t kBlockSize = 4 * 1024;
  const int total_ops = kProducerThreads * kOpsPerProducer;
  state->remaining_reads.store(total_ops, std::memory_order_release);

  file->OpenAsync(path.string(),
                  AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways,
                  bg_runner_,
                  [state](bool success, AsyncFile::Error error) {
                    state->open_ok.store(success && error.ok(), std::memory_order_release);
                    state->open_done.Signal();
                  });
  ASSERT_TRUE(state->open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->open_ok.load(std::memory_order_acquire));

  auto producer = [state, &file](int producer_idx) {
    for (int i = 0; i < kOpsPerProducer; ++i) {
      const int op_idx = producer_idx * kOpsPerProducer + i;
      const std::int64_t offset = static_cast<std::int64_t>(op_idx) * static_cast<std::int64_t>(kBlockSize);
      std::vector<std::uint8_t> payload = MakePayload(kBlockSize, static_cast<std::uint8_t>(op_idx & 0xFF));

      const bool write_accepted =
          IssueWrite(file,
                     offset,
                     payload,
                     [state, file, offset, payload = std::move(payload)](
                         bool write_success, std::size_t bytes_written, AsyncFile::Error write_error) mutable {
                       (void)bytes_written;
                       (void)write_error;
                       if (!write_success) {
                         if (state->remaining_reads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                           state->all_done.Signal();
                         }
                         return;
                       }

                       const bool read_accepted =
                           IssueRead(file,
                                     offset,
                                     payload.size(),
                                     [state, payload = std::move(payload)](bool read_success,
                                                                           std::vector<std::uint8_t> &&data,
                                                                           AsyncFile::Error read_error) mutable {
                                       (void)read_success;
                                       (void)data;
                                       (void)read_error;
                                       (void)payload;
                                       if (state->remaining_reads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                         state->all_done.Signal();
                                       }
                                     });
                       if (!read_accepted && state->remaining_reads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                         state->all_done.Signal();
                       }
                     });

      if (!write_accepted && state->remaining_reads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        state->all_done.Signal();
      }
    }
  };

  std::vector<std::thread> producers;
  producers.reserve(kProducerThreads);
  for (int i = 0; i < kProducerThreads; ++i) {
    producers.emplace_back(producer, i);
  }
  for (auto &t : producers) {
    t.join();
  }

  ASSERT_TRUE(state->all_done.TimedWait(std::chrono::seconds(60)));

  file->CloseAsync([state]() { state->close_barrier.Signal(); });
  ASSERT_TRUE(state->close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixStressTest, CloseRaceCancelsInFlightOperationsWithoutHang) {
  const std::filesystem::path path = NewTempPath("stress_close_race");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  // Heap-allocated state captured BY VALUE in async callbacks so late
  // completions never signal a destroyed WaitableEvent (TSan-confirmed).
  struct State {
    WaitableEvent open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent callbacks_done{WaitableEvent::ResetPolicy::kManual, false};
    WaitableEvent close_issued{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent close_barrier{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> open_ok{false};
    std::atomic<bool> done_submitting{false};
    std::atomic<int> accepted_ops{0};
    std::atomic<int> callback_ops{0};
    std::atomic<bool> bad_outcome{false};
    std::atomic<std::uint32_t> first_bad_native{0};
    std::atomic<int> first_bad_code{static_cast<int>(AsyncFile::ErrorCode::kOk)};
  };

  auto state = std::make_shared<State>();

  // This case is intended to verify close-race liveness/callback completion,
  // not sustained throughput. Keep the workload moderate so Debug-on-WSL over
  // /mnt/c stays focused on cancellation semantics rather than raw I/O speed.
  constexpr int kProducerThreads = 2;
  constexpr int kOpsPerProducer = 8;
  constexpr std::size_t kIoSize = 16 * 1024;

  auto maybe_signal_done = [state]() {
    if (state->done_submitting.load(std::memory_order_acquire)
        && state->callback_ops.load(std::memory_order_acquire) == state->accepted_ops.load(std::memory_order_acquire)) {
      state->callbacks_done.Signal();
    }
  };

  file->OpenAsync(path.string(),
                  AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways,
                  bg_runner_,
                  [state](bool success, AsyncFile::Error error) {
                    state->open_ok.store(success && error.ok(), std::memory_order_release);
                    state->open_done.Signal();
                  });
  ASSERT_TRUE(state->open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(state->open_ok.load(std::memory_order_acquire));

  auto producer = [state, &file, maybe_signal_done](int producer_idx) {
    for (int i = 0; i < kOpsPerProducer; ++i) {
      const std::int64_t offset =
          static_cast<std::int64_t>((producer_idx * kOpsPerProducer + i) * static_cast<int>(kIoSize));

      if (((producer_idx + i) & 1) == 0) {
        std::vector<std::uint8_t> payload =
            MakePayload(kIoSize, static_cast<std::uint8_t>((producer_idx * 17 + i) & 0xFF));
        const bool accepted =
            IssueWrite(file,
                       offset,
                       std::move(payload),
                       [state, maybe_signal_done](bool success, std::size_t bytes_written, AsyncFile::Error error) {
                         (void)bytes_written;
                         if (!success && error.code != AsyncFile::ErrorCode::kCanceled
                             && error.code != AsyncFile::ErrorCode::kBadFileDescriptor
                             && error.code != AsyncFile::ErrorCode::kIoError) {
                           state->first_bad_native.store(error.native_code, std::memory_order_release);
                           state->first_bad_code.store(static_cast<int>(error.code), std::memory_order_release);
                           state->bad_outcome.store(true, std::memory_order_release);
                         }
                         state->callback_ops.fetch_add(1, std::memory_order_acq_rel);
                         maybe_signal_done();
                       });
        if (accepted) {
          state->accepted_ops.fetch_add(1, std::memory_order_acq_rel);
        }
      } else {
        const bool accepted = IssueRead(
            file,
            offset,
            kIoSize,
            [state, maybe_signal_done](bool success, std::vector<std::uint8_t> &&data, AsyncFile::Error error) {
              (void)data;
              if (!success && error.code != AsyncFile::ErrorCode::kCanceled
                  && error.code != AsyncFile::ErrorCode::kBadFileDescriptor
                  && error.code != AsyncFile::ErrorCode::kIoError) {
                state->first_bad_native.store(error.native_code, std::memory_order_release);
                state->first_bad_code.store(static_cast<int>(error.code), std::memory_order_release);
                state->bad_outcome.store(true, std::memory_order_release);
              }
              state->callback_ops.fetch_add(1, std::memory_order_acq_rel);
              maybe_signal_done();
            });
        if (accepted) {
          state->accepted_ops.fetch_add(1, std::memory_order_acq_rel);
        }
      }
    }
  };

  std::vector<std::thread> producers;
  producers.reserve(kProducerThreads);
  for (int i = 0; i < kProducerThreads; ++i) {
    producers.emplace_back(producer, i);
  }

  std::thread closer([state, &file]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    file->CloseAsync([state]() { state->close_issued.Signal(); });
  });

  for (auto &t : producers) {
    t.join();
  }
  state->done_submitting.store(true, std::memory_order_release);
  maybe_signal_done();

  ASSERT_TRUE(state->close_issued.TimedWait(std::chrono::seconds(5)));

  // Join closer before any subsequent ASSERT to prevent std::terminate
  // from an unjoined std::thread on early test exit.
  closer.join();

  ASSERT_TRUE(state->callbacks_done.TimedWait(std::chrono::seconds(30)))
      << "accepted_ops=" << state->accepted_ops.load(std::memory_order_acquire)
      << " callback_ops=" << state->callback_ops.load(std::memory_order_acquire)
      << " bad_outcome=" << state->bad_outcome.load(std::memory_order_acquire)
      << " first_bad_code=" << state->first_bad_code.load(std::memory_order_acquire)
      << " first_bad_native=" << state->first_bad_native.load(std::memory_order_acquire);

  // Load both atomics once to avoid a TOCTOU window between the two
  // loads where a late callback could increment callback_ops.
  int final_callbacks = state->callback_ops.load(std::memory_order_acquire);
  int final_accepted = state->accepted_ops.load(std::memory_order_acquire);
  EXPECT_EQ(final_callbacks, final_accepted);
  EXPECT_FALSE(state->bad_outcome.load(std::memory_order_acquire))
      << "unexpected error code=" << state->first_bad_code.load(std::memory_order_acquire)
      << " native=" << state->first_bad_native.load(std::memory_order_acquire);

  io_runner_->PostTask(FROM_HERE, [state]() { state->close_barrier.Signal(); });
  ASSERT_TRUE(state->close_barrier.TimedWait(std::chrono::seconds(10)));
}

// Analogous to AsyncFileWinTest.LargeReadWriteCallbackDeterminismOnIoThread:
// verifies that open, write, and read callbacks always fire on io_task_runner_
// and are never delivered inline/synchronously during the issuing call.
TEST_F(AsyncFilePosixStressTest, CallbacksAlwaysFireOnIoThread) {
  const std::filesystem::path path = MakeTempFilePath("posix_cb_thread");
  cleanup_files_.push_back(path);
  const std::string path_str = path.string();

  constexpr std::size_t kPayloadSize = 64 * 1024;
  std::vector<std::uint8_t> payload(kPayloadSize);
  for (std::size_t i = 0; i < kPayloadSize; ++i) {
    payload[i] = static_cast<std::uint8_t>(i & 0xFF);
  }

  // Heap-allocated state captured BY VALUE in async callbacks so late
  // completions never signal a destroyed WaitableEvent (TSan-confirmed:
  // done may be signaled from several CloseAsync paths, and the first
  // signal wakes the main thread which then unwinds the test body).
  struct State {
    WaitableEvent done{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> ok{true};
    std::atomic<bool> callback_on_io_thread{true};
    std::atomic<bool> callback_inline_violation{false};
    std::atomic<PlatformThread::PlatformThreadId> io_tid{0};
  };

  auto state = std::make_shared<State>();
  // Local copies so the posted lambda below never captures `this` (the
  // fixture); scoped_refptr keeps the runners alive even after teardown.
  auto io = io_runner_;
  auto bg = bg_runner_;

  io_runner_->PostTask(FROM_HERE, [state, path_str, payload, io, bg]() mutable {
    state->io_tid.store(PlatformThread::CurrentId(), std::memory_order_release);

    auto file = std::make_shared<AsyncFilePosix>(io);

    auto open_called = std::make_shared<std::atomic<bool>>(false);
    file->OpenAsync(path_str,
                    AsyncFile::OpenMode::kReadWrite,
                    AsyncFile::OpenDisposition::kCreateAlways,
                    bg,
                    [state, file, payload, open_called](bool open_success, AsyncFile::Error open_error) mutable {
                      open_called->store(true, std::memory_order_release);
                      if (PlatformThread::CurrentId() != state->io_tid.load(std::memory_order_acquire)) {
                        state->callback_on_io_thread.store(false, std::memory_order_release);
                      }
                      if (!open_success || !open_error.ok()) {
                        state->ok.store(false, std::memory_order_release);
                        file->CloseAsync([state]() { state->done.Signal(); });
                        return;
                      }

                      auto write_called = std::make_shared<std::atomic<bool>>(false);
                      const bool write_accepted = IssueWrite(
                          file,
                          0,
                          payload,
                          [state, file, payload, write_called](
                              bool write_success, std::size_t bytes_written, AsyncFile::Error write_error) mutable {
                            write_called->store(true, std::memory_order_release);
                            if (PlatformThread::CurrentId() != state->io_tid.load(std::memory_order_acquire)) {
                              state->callback_on_io_thread.store(false, std::memory_order_release);
                            }
                            if (!write_success || !write_error.ok() || bytes_written != payload.size()) {
                              state->ok.store(false, std::memory_order_release);
                              file->CloseAsync([state]() { state->done.Signal(); });
                              return;
                            }

                            auto read_called = std::make_shared<std::atomic<bool>>(false);
                            const bool read_accepted = IssueRead(
                                file,
                                0,
                                payload.size(),
                                [state, file, payload, read_called](bool read_success,
                                                                    std::vector<std::uint8_t> &&data,
                                                                    AsyncFile::Error read_error) mutable {
                                  read_called->store(true, std::memory_order_release);
                                  if (PlatformThread::CurrentId() != state->io_tid.load(std::memory_order_acquire)) {
                                    state->callback_on_io_thread.store(false, std::memory_order_release);
                                  }
                                  if (!read_success || !read_error.ok() || data != payload) {
                                    state->ok.store(false, std::memory_order_release);
                                  }
                                  file->CloseAsync([state]() { state->done.Signal(); });
                                });

                            if (!read_accepted) {
                              state->ok.store(false, std::memory_order_release);
                              file->CloseAsync([state]() { state->done.Signal(); });
                              return;
                            }
                            // With PostReadCallback the read callback must not fire
                            // inline; it is always delivered as a fresh io task.
                            if (read_called->load(std::memory_order_acquire)) {
                              state->callback_inline_violation.store(true, std::memory_order_release);
                            }
                          });

                      if (!write_accepted) {
                        state->ok.store(false, std::memory_order_release);
                        file->CloseAsync([state]() { state->done.Signal(); });
                        return;
                      }
                      // Write callback must not fire inline.
                      if (write_called->load(std::memory_order_acquire)) {
                        state->callback_inline_violation.store(true, std::memory_order_release);
                      }
                    });

    // Open callback must not fire inline.
    if (open_called->load(std::memory_order_acquire)) {
      state->callback_inline_violation.store(true, std::memory_order_release);
    }
  });

  ASSERT_TRUE(state->done.TimedWait(std::chrono::seconds(30)));

  EXPECT_TRUE(state->ok.load(std::memory_order_acquire));
  EXPECT_TRUE(state->callback_on_io_thread.load(std::memory_order_acquire));
  EXPECT_FALSE(state->callback_inline_violation.load(std::memory_order_acquire));
}

TEST_F(AsyncFilePosixTest, RaiiDestructionDoesNotHangAndDataIsFlushed) {
  const std::filesystem::path path = NewTempPath("raii_destruct");
  const std::string path_utf8 = PathToUTF8(path);
  const std::vector<std::uint8_t> payload = MakePayload(4096, 0xAB);

  // Scope block: create, open, write, then release  --  RAII destruction.
  {
    auto file = std::make_shared<AsyncFilePosix>(io_runner_);
    ASSERT_TRUE(file);

    // Heap-allocated state captured BY VALUE so a late write completion never
    // signals a destroyed WaitableEvent after this scope unwinds (TSan-confirmed).
    struct Scope1State {
      WaitableEvent open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
      WaitableEvent write_done{WaitableEvent::ResetPolicy::kAutomatic, false};
      std::atomic<bool> open_ok{false};
      std::atomic<bool> write_ok{false};
    };

    auto s1 = std::make_shared<Scope1State>();
    file->OpenAsync(path_utf8,
                    AsyncFile::OpenMode::kReadWrite,
                    AsyncFile::OpenDisposition::kCreateAlways,
                    bg_runner_,
                    [s1](bool success, AsyncFile::Error error) {
                      s1->open_ok.store(success && error.ok(), std::memory_order_release);
                      s1->open_done.Signal();
                    });
    ASSERT_TRUE(s1->open_done.TimedWait(std::chrono::seconds(10)));
    ASSERT_TRUE(s1->open_ok.load(std::memory_order_acquire));

    auto buf = MakeRefCounted<IOBufferWithSize>(payload.size());
    std::memcpy(buf->data(), payload.data(), payload.size());
    const bool accepted =
        IssueWrite(file, 0, payload, [s1, payload](bool success, std::size_t wrote, AsyncFile::Error error) {
          s1->write_ok.store(success && error.ok() && wrote == payload.size(), std::memory_order_release);
          s1->write_done.Signal();
        });
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(s1->write_done.TimedWait(std::chrono::seconds(10)));
    ASSERT_TRUE(s1->write_ok.load(std::memory_order_acquire));

    // file.reset() happens here  --  must not hang or crash.
  }

  // Let the IO thread process the orphan's DeleteSoon task.
  auto settle = std::make_shared<WaitableEvent>(WaitableEvent::ResetPolicy::kAutomatic, false);
  io_runner_->PostTask(FROM_HERE, [settle]() { settle->Signal(); });
  ASSERT_TRUE(settle->TimedWait(std::chrono::seconds(10)));

  // Verify data was persisted.
  auto reader = std::make_shared<AsyncFilePosix>(io_runner_);

  struct ReaderState {
    WaitableEvent read_open_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent read_done{WaitableEvent::ResetPolicy::kAutomatic, false};
    WaitableEvent close_barrier{WaitableEvent::ResetPolicy::kAutomatic, false};
    std::atomic<bool> read_open_ok{false};
    std::atomic<bool> read_ok{false};
  };

  auto rs = std::make_shared<ReaderState>();

  reader->OpenAsync(path_utf8,
                    AsyncFile::OpenMode::kReadOnly,
                    AsyncFile::OpenDisposition::kOpenExisting,
                    bg_runner_,
                    [rs](bool success, AsyncFile::Error error) {
                      rs->read_open_ok.store(success && error.ok(), std::memory_order_release);
                      rs->read_open_done.Signal();
                    });
  ASSERT_TRUE(rs->read_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(rs->read_open_ok.load(std::memory_order_acquire));

  auto rbuf = scoped_refptr<IOBuffer>(new IOBufferWithSize(payload.size()));
  reader->ReadAsync(rbuf, payload.size(), 0, [rs, payload](bool success, std::size_t n, AsyncFile::Error error) {
    rs->read_ok.store(success && error.ok() && n == payload.size(), std::memory_order_release);
    rs->read_done.Signal();
  });
  ASSERT_TRUE(rs->read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(rs->read_ok.load(std::memory_order_acquire));

  std::vector<std::uint8_t> read_back(payload.size());
  std::memcpy(read_back.data(), rbuf->data(), payload.size());
  EXPECT_EQ(read_back, payload);

  reader->CloseAsync([rs]() { rs->close_barrier.Signal(); });
  ASSERT_TRUE(rs->close_barrier.TimedWait(std::chrono::seconds(10)));
}

} // namespace
} // namespace nei

#endif // !defined(_WIN32)
