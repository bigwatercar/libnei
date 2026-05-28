#include <gtest/gtest.h>

#if !defined(_WIN32)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>

#include <neixx/common/location.h>
#include <async_file_posix.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread.h>

namespace nei {
namespace {

std::filesystem::path MakeTempFilePath(const char* name_hint) {
  const auto ticks =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::filesystem::path p = std::filesystem::temp_directory_path();
  p /= std::string("nei_async_file_posix_") + name_hint + "_" +
       std::to_string(static_cast<long long>(ticks)) + ".bin";
  return p;
}

std::vector<std::uint8_t> MakePayload(std::size_t size, std::uint8_t salt) {
  std::vector<std::uint8_t> data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<std::uint8_t>((i * 131u + salt) & 0xFFu);
  }
  return data;
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

    for (const auto& path : cleanup_files_) {
      std::error_code ec;
      (void)std::filesystem::remove(path, ec);
    }
  }

  std::filesystem::path NewTempPath(const char* hint) {
    std::filesystem::path path = MakeTempFilePath(hint);
    cleanup_files_.push_back(path);
    return path;
  }

  Thread io_thread_{"async-file-posix-io"};
  Thread background_thread_{"async-file-posix-bg"};
  scoped_refptr<TaskRunner> io_runner_;
  scoped_refptr<TaskRunner> bg_runner_;
  std::vector<std::filesystem::path> cleanup_files_;
};

class AsyncFilePosixStressTest : public AsyncFilePosixTest {};

TEST_F(AsyncFilePosixTest, OpenFailureRollsBackAndAllowsRetry) {
  const std::filesystem::path missing_path = NewTempPath("open_retry_missing");
  std::error_code ec;
  (void)std::filesystem::remove(missing_path, ec);

  auto file = std::make_shared<AsyncFilePosix>(io_runner_);
  WaitableEvent first_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent second_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> first_ok{true};
  std::atomic<bool> second_ok{false};
  std::uint32_t first_error = 0;

  file->OpenAsync(missing_path.string(), AsyncFile::OpenMode::kReadOnly,
                  AsyncFile::OpenDisposition::kOpenExisting, bg_runner_,
                  [&](bool success, std::uint32_t error_code) {
                    first_ok.store(!success, std::memory_order_release);
                    first_error = error_code;
                    first_open_done.Signal();
                  });

  ASSERT_TRUE(first_open_done.TimedWait(std::chrono::seconds(10)));
  EXPECT_TRUE(first_ok.load(std::memory_order_acquire));
  EXPECT_EQ(first_error, static_cast<std::uint32_t>(ENOENT));
  EXPECT_FALSE(file->is_open());

  file->OpenAsync(missing_path.string(), AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner_,
                  [&](bool success, std::uint32_t error_code) {
                    second_ok.store(success && error_code == 0,
                                    std::memory_order_release);
                    second_open_done.Signal();
                  });

  ASSERT_TRUE(second_open_done.TimedWait(std::chrono::seconds(10)));
  EXPECT_TRUE(second_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(file->is_open());

  file->Close();
  io_runner_->PostTask(FROM_HERE, [&]() { close_barrier.Signal(); });
  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixTest, PositionalConcurrentWritesAndReadbackAreStable) {
  const std::filesystem::path path = NewTempPath("positional_rw");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write_done(WaitableEvent::ResetPolicy::kManual, false);
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> open_ok{false};
  std::atomic<bool> write_ok{true};
  std::atomic<int> pending_writes{2};
  std::atomic<bool> read_ok{false};
  std::vector<std::uint8_t> read_back;

  const std::vector<std::uint8_t> payload_a = MakePayload(64 * 1024, 11);
  const std::vector<std::uint8_t> payload_b = MakePayload(64 * 1024, 29);
  const std::int64_t offset_a = 0;
  const std::int64_t offset_b = 256 * 1024;

  file->OpenAsync(path.string(), AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner_,
                  [&](bool success, std::uint32_t error_code) {
                    open_ok.store(success && error_code == 0,
                                  std::memory_order_release);
                    open_done.Signal();
                  });
  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

  auto launch_write = [&](std::int64_t offset, std::vector<std::uint8_t> payload) {
    const std::size_t expect_bytes = payload.size();
    const bool accepted = file->AsyncWrite(
        offset, std::move(payload),
        [&](bool success, std::size_t bytes_written, std::uint32_t error_code) {
          if (!success || error_code != 0 || bytes_written != expect_bytes) {
            write_ok.store(false, std::memory_order_release);
          }
          if (pending_writes.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            write_done.Signal();
          }
        });

    if (!accepted) {
      write_ok.store(false, std::memory_order_release);
      if (pending_writes.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        write_done.Signal();
      }
    }
  };

  std::thread t1([&]() { launch_write(offset_a, payload_a); });
  std::thread t2([&]() { launch_write(offset_b, payload_b); });
  t1.join();
  t2.join();

  ASSERT_TRUE(write_done.TimedWait(std::chrono::seconds(15)));
  ASSERT_TRUE(write_ok.load(std::memory_order_acquire));

  const std::size_t read_size = static_cast<std::size_t>(offset_b) + payload_b.size();
  const bool read_accepted = file->AsyncRead(
      0, read_size,
      [&](bool success, std::vector<std::uint8_t>&& data, std::uint32_t error_code) {
        read_ok.store(success && error_code == 0, std::memory_order_release);
        read_back = std::move(data);
        read_done.Signal();
      });
  ASSERT_TRUE(read_accepted);
  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(15)));
  ASSERT_TRUE(read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(read_back.size(), read_size);

  ASSERT_TRUE(std::equal(payload_a.begin(), payload_a.end(), read_back.begin()));
  for (std::size_t i = payload_a.size(); i < static_cast<std::size_t>(offset_b); ++i) {
    ASSERT_EQ(read_back[i], 0u);
  }
  ASSERT_TRUE(std::equal(payload_b.begin(), payload_b.end(),
                         read_back.begin() + static_cast<std::ptrdiff_t>(offset_b)));

  file->Close();
  io_runner_->PostTask(FROM_HERE, [&]() { close_barrier.Signal(); });
  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixTest, AppendModeAppendsIgnoringCallerOffset) {
  const std::filesystem::path path = NewTempPath("append");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write1_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write2_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> open_ok{false};
  std::atomic<bool> write1_ok{false};
  std::atomic<bool> write2_ok{false};
  std::atomic<bool> read_open_ok{false};
  std::atomic<bool> read_ok{false};

  const std::vector<std::uint8_t> payload1 = MakePayload(32 * 1024, 3);
  const std::vector<std::uint8_t> payload2 = MakePayload(32 * 1024, 7);
  std::vector<std::uint8_t> all_data;

  file->OpenAsync(path.string(), AsyncFile::OpenMode::kAppend,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner_,
                  [&](bool success, std::uint32_t error_code) {
                    open_ok.store(success && error_code == 0,
                                  std::memory_order_release);
                    open_done.Signal();
                  });
  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(file->AsyncWrite(
      0, payload1,
      [&](bool success, std::size_t bytes_written, std::uint32_t error_code) {
        write1_ok.store(success && error_code == 0 && bytes_written == payload1.size(),
                        std::memory_order_release);
        write1_done.Signal();
      }));
  ASSERT_TRUE(write1_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write1_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(file->AsyncWrite(
      0, payload2,
      [&](bool success, std::size_t bytes_written, std::uint32_t error_code) {
        write2_ok.store(success && error_code == 0 && bytes_written == payload2.size(),
                        std::memory_order_release);
        write2_done.Signal();
      }));
  ASSERT_TRUE(write2_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write2_ok.load(std::memory_order_acquire));

  file->Close();
  io_runner_->PostTask(FROM_HERE, [&]() { close_barrier.Signal(); });
  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));

  auto reader = std::make_shared<AsyncFilePosix>(io_runner_);
  reader->OpenAsync(path.string(), AsyncFile::OpenMode::kReadOnly,
                    AsyncFile::OpenDisposition::kOpenExisting, bg_runner_,
                    [&](bool success, std::uint32_t error_code) {
                      read_open_ok.store(success && error_code == 0,
                                         std::memory_order_release);
                      read_open_done.Signal();
                    });
  ASSERT_TRUE(read_open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_open_ok.load(std::memory_order_acquire));

  const std::size_t total_size = payload1.size() + payload2.size();
  ASSERT_TRUE(reader->AsyncRead(
      0, total_size,
      [&](bool success, std::vector<std::uint8_t>&& data, std::uint32_t error_code) {
        read_ok.store(success && error_code == 0, std::memory_order_release);
        all_data = std::move(data);
        read_done.Signal();
      }));
  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(all_data.size(), total_size);
  ASSERT_TRUE(std::equal(payload1.begin(), payload1.end(), all_data.begin()));
  ASSERT_TRUE(std::equal(payload2.begin(), payload2.end(),
                         all_data.begin() + static_cast<std::ptrdiff_t>(payload1.size())));
}

TEST_F(AsyncFilePosixTest, ReadPastEndReturnsPartialDataAsSuccess) {
  const std::filesystem::path path = NewTempPath("eof_partial");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  const std::vector<std::uint8_t> payload = MakePayload(4096, 91);
  std::atomic<bool> open_ok{false};
  std::atomic<bool> write_ok{false};
  std::atomic<bool> read_ok{false};
  std::vector<std::uint8_t> out;

  file->OpenAsync(path.string(), AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner_,
                  [&](bool success, std::uint32_t error_code) {
                    open_ok.store(success && error_code == 0,
                                  std::memory_order_release);
                    open_done.Signal();
                  });
  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(file->AsyncWrite(
      0, payload,
      [&](bool success, std::size_t bytes_written, std::uint32_t error_code) {
        write_ok.store(success && error_code == 0 && bytes_written == payload.size(),
                       std::memory_order_release);
        write_done.Signal();
      }));
  ASSERT_TRUE(write_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(file->AsyncRead(
      0, payload.size() * 4,
      [&](bool success, std::vector<std::uint8_t>&& data, std::uint32_t error_code) {
        read_ok.store(success && error_code == 0, std::memory_order_release);
        out = std::move(data);
        read_done.Signal();
      }));
  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(out, payload);

  file->Close();
  io_runner_->PostTask(FROM_HERE, [&]() { close_barrier.Signal(); });
  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixTest, LargeOffsetRandomAccessBeyond2GBWorks) {
  const std::filesystem::path path = NewTempPath("large_offset");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent read_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> open_ok{false};
  std::atomic<bool> write_ok{false};
  std::atomic<bool> read_ok{false};

  const std::int64_t large_offset = static_cast<std::int64_t>(3LL * 1024 * 1024 * 1024 + 123);
  const std::vector<std::uint8_t> payload = MakePayload(256, 55);
  std::vector<std::uint8_t> out;

  file->OpenAsync(path.string(), AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner_,
                  [&](bool success, std::uint32_t error_code) {
                    open_ok.store(success && error_code == 0,
                                  std::memory_order_release);
                    open_done.Signal();
                  });
  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(file->AsyncWrite(
      large_offset, payload,
      [&](bool success, std::size_t bytes_written, std::uint32_t error_code) {
        write_ok.store(success && error_code == 0 && bytes_written == payload.size(),
                       std::memory_order_release);
        write_done.Signal();
      }));
  ASSERT_TRUE(write_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(write_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(file->AsyncRead(
      large_offset, payload.size(),
      [&](bool success, std::vector<std::uint8_t>&& data, std::uint32_t error_code) {
        read_ok.store(success && error_code == 0, std::memory_order_release);
        out = std::move(data);
        read_done.Signal();
      }));
  ASSERT_TRUE(read_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(read_ok.load(std::memory_order_acquire));
  ASSERT_EQ(out, payload);

  file->Close();
  io_runner_->PostTask(FROM_HERE, [&]() { close_barrier.Signal(); });
  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixStressTest, HighConcurrencyMixedReadWriteChain) {
  const std::filesystem::path path = NewTempPath("stress_mixed_rw");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kManual, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> open_ok{false};
  std::atomic<int> remaining_reads{0};

  constexpr int kProducerThreads = 4;
  constexpr int kOpsPerProducer = 10;
  constexpr std::size_t kBlockSize = 4 * 1024;
  const int total_ops = kProducerThreads * kOpsPerProducer;
  remaining_reads.store(total_ops, std::memory_order_release);

  file->OpenAsync(path.string(), AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner_,
                  [&](bool success, std::uint32_t error_code) {
                    open_ok.store(success && error_code == 0,
                                  std::memory_order_release);
                    open_done.Signal();
                  });
  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

  auto producer = [&](int producer_idx) {
    for (int i = 0; i < kOpsPerProducer; ++i) {
      const int op_idx = producer_idx * kOpsPerProducer + i;
      const std::int64_t offset =
          static_cast<std::int64_t>(op_idx) * static_cast<std::int64_t>(kBlockSize);
      std::vector<std::uint8_t> payload =
          MakePayload(kBlockSize, static_cast<std::uint8_t>(op_idx & 0xFF));

      const bool write_accepted = file->AsyncWrite(
          offset, payload,
          [&, file, offset, payload = std::move(payload)](bool write_success,
                                                          std::size_t bytes_written,
                                                          std::uint32_t write_error) mutable {
            (void)bytes_written;
            (void)write_error;
            if (!write_success) {
              if (remaining_reads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                all_done.Signal();
              }
              return;
            }

            const bool read_accepted = file->AsyncRead(
                offset, payload.size(),
                [&, payload = std::move(payload)](bool read_success,
                                                 std::vector<std::uint8_t>&& data,
                                                 std::uint32_t read_error) mutable {
                  (void)read_success;
                  (void)data;
                  (void)read_error;
                  (void)payload;
                  if (remaining_reads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    all_done.Signal();
                  }
                });
            if (!read_accepted &&
                remaining_reads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
              all_done.Signal();
            }
          });

      if (!write_accepted &&
          remaining_reads.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        all_done.Signal();
      }
    }
  };

  std::vector<std::thread> producers;
  producers.reserve(kProducerThreads);
  for (int i = 0; i < kProducerThreads; ++i) {
    producers.emplace_back(producer, i);
  }
  for (auto& t : producers) {
    t.join();
  }

  ASSERT_TRUE(all_done.TimedWait(std::chrono::seconds(60)));

  file->Close();
  io_runner_->PostTask(FROM_HERE, [&]() { close_barrier.Signal(); });
  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));
}

TEST_F(AsyncFilePosixStressTest, CloseRaceCancelsInFlightOperationsWithoutHang) {
  const std::filesystem::path path = NewTempPath("stress_close_race");
  auto file = std::make_shared<AsyncFilePosix>(io_runner_);

  WaitableEvent open_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent callbacks_done(WaitableEvent::ResetPolicy::kManual, false);
  WaitableEvent close_issued(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent close_barrier(WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> open_ok{false};
  std::atomic<bool> done_submitting{false};
  std::atomic<int> accepted_ops{0};
  std::atomic<int> callback_ops{0};
  std::atomic<bool> bad_outcome{false};

  constexpr int kProducerThreads = 6;
  constexpr int kOpsPerProducer = 20;
  constexpr std::size_t kIoSize = 128 * 1024;

  auto maybe_signal_done = [&]() {
    if (done_submitting.load(std::memory_order_acquire) &&
        callback_ops.load(std::memory_order_acquire) ==
            accepted_ops.load(std::memory_order_acquire)) {
      callbacks_done.Signal();
    }
  };

  file->OpenAsync(path.string(), AsyncFile::OpenMode::kReadWrite,
                  AsyncFile::OpenDisposition::kCreateAlways, bg_runner_,
                  [&](bool success, std::uint32_t error_code) {
                    open_ok.store(success && error_code == 0,
                                  std::memory_order_release);
                    open_done.Signal();
                  });
  ASSERT_TRUE(open_done.TimedWait(std::chrono::seconds(10)));
  ASSERT_TRUE(open_ok.load(std::memory_order_acquire));

  auto producer = [&](int producer_idx) {
    for (int i = 0; i < kOpsPerProducer; ++i) {
      const std::int64_t offset =
          static_cast<std::int64_t>((producer_idx * kOpsPerProducer + i) *
                                    static_cast<int>(kIoSize));

      if (((producer_idx + i) & 1) == 0) {
        std::vector<std::uint8_t> payload =
            MakePayload(kIoSize, static_cast<std::uint8_t>((producer_idx * 17 + i) & 0xFF));
        const bool accepted = file->AsyncWrite(
            offset, std::move(payload),
            [&](bool success, std::size_t bytes_written, std::uint32_t error_code) {
              (void)bytes_written;
              if (!success && error_code != static_cast<std::uint32_t>(ECANCELED) &&
                  error_code != static_cast<std::uint32_t>(EBADF) &&
                  error_code != static_cast<std::uint32_t>(EIO)) {
                bad_outcome.store(true, std::memory_order_release);
              }
              callback_ops.fetch_add(1, std::memory_order_acq_rel);
              maybe_signal_done();
            });
        if (accepted) {
          accepted_ops.fetch_add(1, std::memory_order_acq_rel);
        }
      } else {
        const bool accepted = file->AsyncRead(
            offset, kIoSize,
            [&](bool success, std::vector<std::uint8_t>&& data,
                std::uint32_t error_code) {
              (void)data;
              if (!success && error_code != static_cast<std::uint32_t>(ECANCELED) &&
                  error_code != static_cast<std::uint32_t>(EBADF) &&
                  error_code != static_cast<std::uint32_t>(EIO)) {
                bad_outcome.store(true, std::memory_order_release);
              }
              callback_ops.fetch_add(1, std::memory_order_acq_rel);
              maybe_signal_done();
            });
        if (accepted) {
          accepted_ops.fetch_add(1, std::memory_order_acq_rel);
        }
      }
    }
  };

  std::vector<std::thread> producers;
  producers.reserve(kProducerThreads);
  for (int i = 0; i < kProducerThreads; ++i) {
    producers.emplace_back(producer, i);
  }

  std::thread closer([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    file->Close();
    close_issued.Signal();
  });

  for (auto& t : producers) {
    t.join();
  }
  done_submitting.store(true, std::memory_order_release);
  maybe_signal_done();

  ASSERT_TRUE(close_issued.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(callbacks_done.TimedWait(std::chrono::seconds(30)));

  closer.join();

  EXPECT_EQ(callback_ops.load(std::memory_order_acquire),
            accepted_ops.load(std::memory_order_acquire));
  EXPECT_FALSE(bad_outcome.load(std::memory_order_acquire));

  io_runner_->PostTask(FROM_HERE, [&]() { close_barrier.Signal(); });
  ASSERT_TRUE(close_barrier.TimedWait(std::chrono::seconds(10)));
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

  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{true};
  std::atomic<bool> callback_on_io_thread{true};
  std::atomic<bool> callback_inline_violation{false};
  std::atomic<PlatformThread::PlatformThreadId> io_tid{0};

  io_runner_->PostTask(
      FROM_HERE,
      [&, path_str, payload]() mutable {
        io_tid.store(PlatformThread::CurrentId(), std::memory_order_release);

        auto file = std::make_shared<AsyncFilePosix>(io_runner_);

        auto open_called = std::make_shared<std::atomic<bool>>(false);
        file->OpenAsync(
            path_str, AsyncFile::OpenMode::kReadWrite,
            AsyncFile::OpenDisposition::kCreateAlways, bg_runner_,
            [&, file, payload, open_called](bool open_success,
                                            std::uint32_t open_error) mutable {
              open_called->store(true, std::memory_order_release);
              if (PlatformThread::CurrentId() !=
                  io_tid.load(std::memory_order_acquire)) {
                callback_on_io_thread.store(false, std::memory_order_release);
              }
              if (!open_success || open_error != 0) {
                ok.store(false, std::memory_order_release);
                file->Close();
                done.Signal();
                return;
              }

              auto write_called = std::make_shared<std::atomic<bool>>(false);
              const bool write_accepted = file->AsyncWrite(
                  0, payload,
                  [&, file, payload, write_called](
                      bool write_success, std::size_t bytes_written,
                      std::uint32_t write_error) mutable {
                    write_called->store(true, std::memory_order_release);
                    if (PlatformThread::CurrentId() !=
                        io_tid.load(std::memory_order_acquire)) {
                      callback_on_io_thread.store(false,
                                                  std::memory_order_release);
                    }
                    if (!write_success || write_error != 0 ||
                        bytes_written != payload.size()) {
                      ok.store(false, std::memory_order_release);
                      file->Close();
                      done.Signal();
                      return;
                    }

                    auto read_called = std::make_shared<std::atomic<bool>>(false);
                    const bool read_accepted = file->AsyncRead(
                        0, payload.size(),
                        [&, file, payload, read_called](
                            bool read_success,
                            std::vector<std::uint8_t>&& data,
                            std::uint32_t read_error) mutable {
                          read_called->store(true, std::memory_order_release);
                          if (PlatformThread::CurrentId() !=
                              io_tid.load(std::memory_order_acquire)) {
                            callback_on_io_thread.store(
                                false, std::memory_order_release);
                          }
                          if (!read_success || read_error != 0 ||
                              data != payload) {
                            ok.store(false, std::memory_order_release);
                          }
                          file->Close();
                          done.Signal();
                        });

                    if (!read_accepted) {
                      ok.store(false, std::memory_order_release);
                      file->Close();
                      done.Signal();
                      return;
                    }
                    // With PostReadCallback the read callback must not fire
                    // inline; it is always delivered as a fresh io task.
                    if (read_called->load(std::memory_order_acquire)) {
                      callback_inline_violation.store(true,
                                                      std::memory_order_release);
                    }
                  });

              if (!write_accepted) {
                ok.store(false, std::memory_order_release);
                file->Close();
                done.Signal();
                return;
              }
              // Write callback must not fire inline.
              if (write_called->load(std::memory_order_acquire)) {
                callback_inline_violation.store(true, std::memory_order_release);
              }
            });

        // Open callback must not fire inline.
        if (open_called->load(std::memory_order_acquire)) {
          callback_inline_violation.store(true, std::memory_order_release);
        }
      });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(30)));

  EXPECT_TRUE(ok.load(std::memory_order_acquire));
  EXPECT_TRUE(callback_on_io_thread.load(std::memory_order_acquire));
  EXPECT_FALSE(callback_inline_violation.load(std::memory_order_acquire));
}

}  // namespace
}  // namespace nei

#endif  // !defined(_WIN32)