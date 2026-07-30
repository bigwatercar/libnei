#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_file.h>
#include <neixx/io/io_buffer.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
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

namespace {

std::filesystem::path MakeTempPath() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto seed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
  std::filesystem::path path = std::filesystem::temp_directory_path();
  path /= std::string("nei_async_file_demo_") + std::to_string(seed) + ".txt";
  return path;
}

// Runs the demo using the platform-agnostic AsyncFile interface.
// Returns true on success.
bool RunDemo(nei::AsyncFile &file,
             const nei::scoped_refptr<nei::TaskRunner> &background_runner,
             const std::filesystem::path &path) {
  const std::string path_str = PathToUTF8(path);
  const std::string payload_text = "Hello from AsyncFile demo (cross-platform).";
  const std::vector<std::uint8_t> payload(payload_text.begin(), payload_text.end());

  nei::WaitableEvent open_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> open_ok{false};
  std::atomic<std::uint32_t> open_error{0};

  file.OpenAsync(path_str,
                 nei::AsyncFile::OpenMode::kReadWrite,
                 nei::AsyncFile::OpenDisposition::kCreateAlways,
                 background_runner,
                 [&](bool success, nei::AsyncFile::Error error) {
                   open_ok.store(success, std::memory_order_release);
                   open_error.store(error.native_code, std::memory_order_release);
                   open_done.Signal();
                 });

  if (!open_done.TimedWait(std::chrono::seconds(10))) {
    std::cerr << "[demo] Timed out waiting for OpenAsync." << std::endl;
    return false;
  }
  if (!open_ok.load(std::memory_order_acquire)) {
    std::cerr << "[demo] OpenAsync failed, error=" << open_error.load(std::memory_order_acquire) << std::endl;
    return false;
  }
  std::cout << "[demo] File opened: " << path_str << std::endl;

  // --- Write ---
  nei::WaitableEvent write_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> write_ok{false};
  std::atomic<std::uint32_t> write_error{0};
  std::atomic<std::size_t> bytes_written{0};

  nei::scoped_refptr<nei::IOBuffer> write_buf(new nei::IOBufferWithSize(payload.size()));
  std::memcpy(write_buf->data(), payload.data(), payload.size());

  file.WriteAsync(write_buf, payload.size(), 0, [&](bool success, std::size_t written, nei::AsyncFile::Error error) {
    write_ok.store(success, std::memory_order_release);
    bytes_written.store(written, std::memory_order_release);
    write_error.store(error.native_code, std::memory_order_release);
    write_done.Signal();
  });

  if (!write_done.TimedWait(std::chrono::seconds(10))) {
    std::cerr << "[demo] Timed out waiting for WriteAsync." << std::endl;
    nei::WaitableEvent close_ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
    file.CloseAsync([&]() { close_ev.Signal(); });
    close_ev.Wait();
    return false;
  }
  if (!write_ok.load(std::memory_order_acquire) || bytes_written.load(std::memory_order_acquire) != payload.size()) {
    std::cerr << "[demo] WriteAsync failed, error=" << write_error.load(std::memory_order_acquire)
              << ", bytes_written=" << bytes_written.load(std::memory_order_acquire) << std::endl;
    nei::WaitableEvent close_ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
    file.CloseAsync([&]() { close_ev.Signal(); });
    close_ev.Wait();
    return false;
  }
  std::cout << "[demo] Wrote " << bytes_written.load(std::memory_order_acquire) << " bytes." << std::endl;

  // --- Read back ---
  nei::WaitableEvent read_done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> read_ok{false};
  std::atomic<std::uint32_t> read_error{0};
  std::atomic<std::size_t> bytes_read{0};

  nei::scoped_refptr<nei::IOBuffer> read_buf(new nei::IOBufferWithSize(payload.size()));

  file.ReadAsync(read_buf, payload.size(), 0, [&](bool success, std::size_t read_now, nei::AsyncFile::Error error) {
    read_ok.store(success, std::memory_order_release);
    bytes_read.store(read_now, std::memory_order_release);
    read_error.store(error.native_code, std::memory_order_release);
    read_done.Signal();
  });

  if (!read_done.TimedWait(std::chrono::seconds(10))) {
    std::cerr << "[demo] Timed out waiting for ReadAsync." << std::endl;
    nei::WaitableEvent close_ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
    file.CloseAsync([&]() { close_ev.Signal(); });
    close_ev.Wait();
    return false;
  }
  const std::size_t read_size = bytes_read.load(std::memory_order_acquire);
  const std::string read_text(reinterpret_cast<const char *>(read_buf->data()), read_size);
  if (!read_ok.load(std::memory_order_acquire) || read_text != payload_text) {
    std::cerr << "[demo] ReadAsync failed, error=" << read_error.load(std::memory_order_acquire)
              << ", size=" << read_size << std::endl;
    nei::WaitableEvent close_ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
    file.CloseAsync([&]() { close_ev.Signal(); });
    close_ev.Wait();
    return false;
  }

  std::cout << "[demo] Read back: " << read_text << std::endl;

  nei::WaitableEvent close_ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  file.CloseAsync([&]() { close_ev.Signal(); });
  close_ev.Wait();
  return true;
}

} // namespace

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
  const nei::scoped_refptr<nei::TaskRunner> background_runner = background_thread.GetTaskRunner();
  if (!io_runner || !background_runner) {
    std::cerr << "Failed to acquire task runners." << std::endl;
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

  const std::filesystem::path path = MakeTempPath();
  auto file = nei::AsyncFile::Create(io_runner);
  if (!file) {
    std::cerr << "Failed to create AsyncFile." << std::endl;
    background_thread.Stop();
    io_thread.Stop();
    return 1;
  }

#if defined(_WIN32)
  std::cout << "[demo] Platform: Windows (AsyncFile factory)" << std::endl;
#else
  std::cout << "[demo] Platform: POSIX (AsyncFile factory)" << std::endl;
#endif

  const bool ok = RunDemo(*file, background_runner, path);

  file.reset(); // trigger Close/cleanup before stopping threads

  background_thread.Stop();
  io_thread.Stop();

  std::error_code ec;
  (void)std::filesystem::remove(path, ec);

  if (!ok) {
    std::cerr << "AsyncFile demo FAILED." << std::endl;
    return 1;
  }

  std::cout << "[demo] AsyncFile demo completed successfully." << std::endl;
  return 0;
}