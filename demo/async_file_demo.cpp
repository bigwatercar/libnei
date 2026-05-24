#include <chrono>
#include <cstdint>
#include <filesystem>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)

#include <neixx/common/location.h>
#include <neixx/io/async_file_win.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/threading/thread.h>

namespace {

std::filesystem::path MakeTempPath() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto seed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());

  std::filesystem::path path = std::filesystem::temp_directory_path();
  path /= std::string("nei_async_file_demo_") + std::to_string(seed) + ".txt";
  return path;
}

}  // namespace

int main() {
  nei::Thread io_thread("async-file-demo-io");
  nei::Thread::Options io_options;
  io_options.message_pump_type = nei::MessagePumpType::IO;
  if (!io_thread.StartWithOptions(io_options)) {
    std::cerr << "Failed to start IO thread." << std::endl;
    return 1;
  }

  nei::Thread background_thread("async-file-demo-bg");
  if (!background_thread.Start()) {
    std::cerr << "Failed to start background thread." << std::endl;
    io_thread.Stop();
    return 1;
  }

  const nei::scoped_refptr<nei::TaskRunner> io_runner = io_thread.GetTaskRunner();
  const nei::scoped_refptr<nei::TaskRunner> background_runner =
      background_thread.GetTaskRunner();
  if (!io_runner || !background_runner) {
    std::cerr << "Failed to acquire task runners." << std::endl;
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  const std::filesystem::path path = MakeTempPath();
  const std::string path_utf8 = path.u8string();
  const std::string payload_text = "Hello from AsyncFileWin demo.";
  const std::vector<std::uint8_t> payload(payload_text.begin(), payload_text.end());
  auto file = std::make_shared<nei::AsyncFileWin>(io_runner);

  nei::WaitableEvent open_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  nei::WaitableEvent write_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  nei::WaitableEvent read_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);

  std::atomic<bool> open_ok{false};
  std::atomic<std::uint32_t> open_error{0};

  std::atomic<bool> write_ok{false};
  std::atomic<std::uint32_t> write_error{0};
  std::atomic<std::size_t> bytes_written{0};

  std::atomic<bool> read_ok{false};
  std::atomic<std::uint32_t> read_error{0};
  std::vector<std::uint8_t> read_data;

  file->OpenAsync(path_utf8,
                  nei::AsyncFile::OpenMode::kReadWrite,
                  nei::AsyncFile::OpenDisposition::kCreateAlways,
                  background_runner,
                  [&](bool success, std::uint32_t error_code) {
                    open_ok.store(success, std::memory_order_release);
                    open_error.store(error_code, std::memory_order_release);
                    open_done.Signal();
                  });

  if (!open_done.TimedWait(std::chrono::milliseconds(10000))) {
    std::cerr << "Timed out waiting for OpenAsync callback." << std::endl;
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  if (!open_ok.load(std::memory_order_acquire)) {
    std::cerr << "OpenAsync failed, error="
              << open_error.load(std::memory_order_acquire) << std::endl;
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  const bool write_accepted = file->AsyncWrite(
      0,
      payload,
      [&](bool success, std::size_t written, std::uint32_t error_code) {
        write_ok.store(success, std::memory_order_release);
        bytes_written.store(written, std::memory_order_release);
        write_error.store(error_code, std::memory_order_release);
        write_done.Signal();
      });

  if (!write_accepted) {
    std::cerr << "AsyncWrite was not accepted." << std::endl;
    file->Close();
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  if (!write_done.TimedWait(std::chrono::milliseconds(10000))) {
    std::cerr << "Timed out waiting for AsyncWrite callback." << std::endl;
    file->Close();
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  if (!write_ok.load(std::memory_order_acquire) ||
      write_error.load(std::memory_order_acquire) != 0 ||
      bytes_written.load(std::memory_order_acquire) != payload.size()) {
    std::cerr << "AsyncWrite failed, error="
              << write_error.load(std::memory_order_acquire)
              << ", bytes_written="
              << bytes_written.load(std::memory_order_acquire) << std::endl;
    file->Close();
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  const bool read_accepted = file->AsyncRead(
      0,
      payload.size(),
      [&](bool success, std::vector<std::uint8_t>&& data, std::uint32_t error_code) {
        read_ok.store(success, std::memory_order_release);
        read_error.store(error_code, std::memory_order_release);
        read_data = std::move(data);
        read_done.Signal();
      });

  if (!read_accepted) {
    std::cerr << "AsyncRead was not accepted." << std::endl;
    file->Close();
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  if (!read_done.TimedWait(std::chrono::milliseconds(10000))) {
    std::cerr << "Timed out waiting for AsyncRead callback." << std::endl;
    file->Close();
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  bool ok = true;
  if (!read_ok.load(std::memory_order_acquire) ||
      read_error.load(std::memory_order_acquire) != 0 || read_data != payload) {
    std::cerr << "AsyncRead failed, error="
              << read_error.load(std::memory_order_acquire)
              << ", size=" << read_data.size() << std::endl;
    ok = false;
  } else {
    const std::string text(read_data.begin(), read_data.end());
    std::cout << "Read back: " << text << std::endl;
  }

  file->Close();

  background_thread.Stop();
  io_thread.Stop();

  std::error_code ec;
  (void)std::filesystem::remove(path, ec);

  if (!ok) {
    return 1;
  }

  std::cout << "AsyncFile demo completed successfully." << std::endl;
  return 0;
}

#else

int main() {
  std::cout << "async_file_demo currently supports Windows AsyncFileWin only."
            << std::endl;
  return 0;
}

#endif