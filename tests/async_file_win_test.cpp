#include <gtest/gtest.h>

#if defined(_WIN32)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_file_win.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/threading/platform_thread.h>
#include <neixx/threading/thread.h>

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

TEST(AsyncFileWinTest, LargeReadWriteCallbackDeterminismOnIoThread) {
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
  const std::string path_utf8 = path.u8string();
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
                                        std::uint32_t open_error) mutable {
          open_called->store(true, std::memory_order_release);
          if (PlatformThread::CurrentId() !=
              io_tid.load(std::memory_order_acquire)) {
            callback_on_io_thread.store(false, std::memory_order_release);
          }
          if (!open_success || open_error != ERROR_SUCCESS) {
            ok.store(false, std::memory_order_release);
            file->Close();
            done.Signal();
            return;
          }

          auto write_called = std::make_shared<std::atomic<bool>>(false);
          const bool write_accepted = file->AsyncWrite(
              0, payload,
              [&, file, payload, write_called](bool write_success,
                                               std::size_t bytes_written,
                                               std::uint32_t write_error) mutable {
                write_called->store(true, std::memory_order_release);
                if (PlatformThread::CurrentId() !=
                    io_tid.load(std::memory_order_acquire)) {
                  callback_on_io_thread.store(false, std::memory_order_release);
                }
                if (!write_success || write_error != ERROR_SUCCESS ||
                    bytes_written != payload.size()) {
                  ok.store(false, std::memory_order_release);
                  file->Close();
                  done.Signal();
                  return;
                }

                auto read_called = std::make_shared<std::atomic<bool>>(false);
                const bool read_accepted = file->AsyncRead(
                    0, payload.size(),
                    [&, file, payload, read_called](bool read_success,
                                                    std::vector<std::uint8_t>&& data,
                                                    std::uint32_t read_error) mutable {
                      read_called->store(true, std::memory_order_release);
                      if (PlatformThread::CurrentId() !=
                          io_tid.load(std::memory_order_acquire)) {
                        callback_on_io_thread.store(false,
                                                    std::memory_order_release);
                      }
                      if (!read_success || read_error != ERROR_SUCCESS ||
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
          if (write_called->load(std::memory_order_acquire)) {
            callback_inline_violation.store(true, std::memory_order_release);
          }
        });

    if (open_called->load(std::memory_order_acquire)) {
      callback_inline_violation.store(true, std::memory_order_release);
    }
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(30)));
  EXPECT_TRUE(ok.load(std::memory_order_acquire));
  EXPECT_TRUE(callback_on_io_thread.load(std::memory_order_acquire));
  EXPECT_FALSE(callback_inline_violation.load(std::memory_order_acquire));

  background_thread.Stop();
  io_thread.Stop();
  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

TEST(AsyncFileWinTest, RepeatedStressMaintainsCallbackThreadDeterminism) {
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
  const std::string path_utf8 = path.u8string();
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
        [&, file](bool open_success, std::uint32_t open_error) mutable {
          if (PlatformThread::CurrentId() !=
              io_tid.load(std::memory_order_acquire)) {
            callback_on_io_thread.store(false, std::memory_order_release);
          }
          if (!open_success || open_error != ERROR_SUCCESS) {
            ok.store(false, std::memory_order_release);
            file->Close();
            done.Signal();
            return;
          }

          auto run_round = std::make_shared<std::function<void(int)>>();
          *run_round = [&, file, run_round](int round) mutable {
            if (round >= kRounds) {
              file->Close();
              done.Signal();
              return;
            }

            const std::vector<std::uint8_t> expected =
                MakePayload(kBytesPerRound, static_cast<std::uint8_t>(round));

            auto write_called = std::make_shared<std::atomic<bool>>(false);
            const bool write_accepted = file->AsyncWrite(
                0, expected,
                [&, file, run_round, round, expected,
                 write_called](bool write_success, std::size_t bytes_written,
                               std::uint32_t write_error) mutable {
                  write_called->store(true, std::memory_order_release);
                  if (PlatformThread::CurrentId() !=
                      io_tid.load(std::memory_order_acquire)) {
                    callback_on_io_thread.store(false,
                                                std::memory_order_release);
                  }
                  if (!write_success || write_error != ERROR_SUCCESS ||
                      bytes_written != expected.size()) {
                    ok.store(false, std::memory_order_release);
                    file->Close();
                    done.Signal();
                    return;
                  }

                  auto read_called = std::make_shared<std::atomic<bool>>(false);
                  const bool read_accepted = file->AsyncRead(
                      0, expected.size(),
                      [&, file, run_round, round, expected,
                       read_called](bool read_success,
                                    std::vector<std::uint8_t>&& data,
                                    std::uint32_t read_error) mutable {
                        read_called->store(true, std::memory_order_release);
                        if (PlatformThread::CurrentId() !=
                            io_tid.load(std::memory_order_acquire)) {
                          callback_on_io_thread.store(false,
                                                      std::memory_order_release);
                        }
                        if (!read_success || read_error != ERROR_SUCCESS ||
                            data != expected) {
                          ok.store(false, std::memory_order_release);
                          file->Close();
                          done.Signal();
                          return;
                        }
                        (*run_round)(round + 1);
                      });

                  if (!read_accepted) {
                    ok.store(false, std::memory_order_release);
                    file->Close();
                    done.Signal();
                    return;
                  }
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
            if (write_called->load(std::memory_order_acquire)) {
              callback_inline_violation.store(true,
                                             std::memory_order_release);
            }
          };

          (*run_round)(0);
        });
  });

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(60)));
  EXPECT_TRUE(ok.load(std::memory_order_acquire));
  EXPECT_TRUE(callback_on_io_thread.load(std::memory_order_acquire));
  EXPECT_FALSE(callback_inline_violation.load(std::memory_order_acquire));

  background_thread.Stop();
  io_thread.Stop();
  std::error_code ec;
  (void)std::filesystem::remove(path, ec);
}

}  // namespace
}  // namespace nei

#endif  // defined(_WIN32)