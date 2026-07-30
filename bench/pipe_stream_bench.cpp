#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
#include <neixx/common/platform_handle.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/pipe_stream.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::high_resolution_clock;

constexpr std::size_t kDefaultTotalBytes = 64 * 1024 * 1024;
constexpr std::size_t kReadBufferSize = 256 * 1024;

struct BufferHolder {
  nei::scoped_refptr<nei::IOBufferWithSize> sized;
  nei::scoped_refptr<nei::IOBuffer> buf;
};

BufferHolder AcquireBuffer(std::size_t size) {
  BufferHolder holder;
  holder.sized = nei::IOBufferPool::GetInstance().AcquireBuffer(size);
  holder.buf = nei::scoped_refptr<nei::IOBuffer>(holder.sized.get());
  return holder;
}

std::string FormatSize(std::size_t bytes) {
  if (bytes >= 1024 * 1024 * 1024) {
    return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
  }
  if (bytes >= 1024 * 1024) {
    return std::to_string(bytes / (1024 * 1024)) + " MB";
  }
  if (bytes >= 1024) {
    return std::to_string(bytes / 1024) + " KB";
  }
  return std::to_string(bytes) + " B";
}

#if defined(_WIN32)
bool CreateAsyncPipePair(nei::PlatformHandle &read_handle, nei::PlatformHandle &write_handle) {
  static std::atomic<unsigned long long> pipe_counter{0};
  const DWORD pid = GetCurrentProcessId();
  const unsigned long long counter = pipe_counter.fetch_add(1, std::memory_order_relaxed);
  const std::string pipe_name = "\\\\.\\pipe\\nei_pipe_stream_bench_" + std::to_string(pid) + "_"
                                + std::to_string(GetTickCount64()) + "_" + std::to_string(counter);

  HANDLE server = CreateNamedPipeA(pipe_name.c_str(),
                                   PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1,
                                   0,
                                   0,
                                   0,
                                   nullptr);
  if (server == INVALID_HANDLE_VALUE) {
    return false;
  }

  HANDLE client = CreateFileA(pipe_name.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                              nullptr);
  if (client == INVALID_HANDLE_VALUE) {
    CloseHandle(server);
    return false;
  }

  const BOOL connected = ConnectNamedPipe(server, nullptr);
  const DWORD error = connected ? ERROR_SUCCESS : GetLastError();
  if (!connected && error != ERROR_PIPE_CONNECTED) {
    CloseHandle(server);
    CloseHandle(client);
    return false;
  }

  read_handle = nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(server);
  write_handle = nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(client);
  return true;
}
#else
bool CreateAsyncPipePair(nei::PlatformHandle &read_handle, nei::PlatformHandle &write_handle) {
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    return false;
  }

  const int flags = fcntl(fds[0], F_GETFL, 0);
  if (flags == -1 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) == -1) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }

  read_handle = nei::PlatformHandle::FromNativeHandle(fds[0]);
  write_handle = nei::PlatformHandle::FromNativeHandle(fds[1]);
  return true;
}
#endif

struct BenchResult {
  std::size_t chunk_size = 0;
  std::size_t total_bytes = 0;
  std::uint64_t elapsed_us = 0;
  double throughput_mb_s = 0.0;
  double chunks_per_second = 0.0;
};

struct PipeBenchState {
  explicit PipeBenchState(nei::scoped_refptr<nei::TaskRunner> runner,
                          std::size_t chunk,
                          std::size_t total,
                          nei::WaitableEvent *done_event)
      : io_runner(std::move(runner))
      , chunk_size(chunk)
      , total_bytes(total)
      , done(done_event) {
  }

  void Start(const std::shared_ptr<PipeBenchState> &self,
             nei::PlatformHandle read_handle,
             nei::PlatformHandle write_handle) {
    input = std::make_shared<nei::PipeInputStream>(io_runner);
    output = std::make_shared<nei::PipeOutputStream>(io_runner);
    if (!input->BindPlatformHandle(std::move(read_handle)) || !output->BindPlatformHandle(std::move(write_handle))) {
      success.store(false, std::memory_order_release);
      done->Signal();
      return;
    }

    payload = AcquireBuffer(chunk_size);
    for (std::size_t i = 0; i < chunk_size; ++i) {
      payload.buf->data()[i] = static_cast<unsigned char>((i * 131u + 17u) & 0xFFu);
    }

    started = Clock::now();
    IssueRead(self);
    IssueWrite(self);
  }

  void CloseAndSignal() {
    if (input) {
      input->Close();
    }
    if (output) {
      output->Close();
    }
    done->Signal();
  }

  static void IssueRead(const std::shared_ptr<PipeBenchState> &self) {
    const std::size_t remaining = self->total_bytes - self->bytes_read;
    if (remaining == 0) {
      self->finished = Clock::now();
      self->success.store(true, std::memory_order_release);
      self->CloseAndSignal();
      return;
    }

    const std::size_t read_size = (std::min)(remaining, kReadBufferSize);
    auto read_holder = AcquireBuffer(read_size);
    self->input->ReadAsync(read_holder.buf, read_size, [self, read_holder](bool ok, std::size_t n) {
      if (!ok || n == 0) {
        self->success.store(false, std::memory_order_release);
        self->CloseAndSignal();
        return;
      }
      self->bytes_read += n;
      IssueRead(self);
    });
  }

  static void IssueWrite(const std::shared_ptr<PipeBenchState> &self) {
    const std::size_t remaining = self->total_bytes - self->bytes_written;
    if (remaining == 0) {
      return;
    }

    const std::size_t write_size = (std::min)(remaining, self->chunk_size);
    self->output->WriteAsync(self->payload.buf, write_size, [self, write_size](bool ok, std::size_t n) {
      if (!ok || n == 0) {
        self->success.store(false, std::memory_order_release);
        self->CloseAndSignal();
        return;
      }
      self->bytes_written += n;
      IssueWrite(self);
    });
  }

  nei::scoped_refptr<nei::TaskRunner> io_runner;
  std::size_t chunk_size = 0;
  std::size_t total_bytes = 0;
  std::size_t bytes_written = 0;
  std::size_t bytes_read = 0;
  std::shared_ptr<nei::PipeInputStream> input;
  std::shared_ptr<nei::PipeOutputStream> output;
  BufferHolder payload;
  Clock::time_point started;
  Clock::time_point finished;
  std::atomic<bool> success{false};
  nei::WaitableEvent *done = nullptr;
};

BenchResult
RunBench(const nei::scoped_refptr<nei::TaskRunner> &io_runner, std::size_t chunk_size, std::size_t total_bytes) {
  nei::PlatformHandle read_handle;
  nei::PlatformHandle write_handle;
  if (!CreateAsyncPipePair(read_handle, write_handle)) {
    return {};
  }

  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  auto state = std::make_shared<PipeBenchState>(io_runner, chunk_size, total_bytes, &done);
  io_runner->PostTask(FROM_HERE,
                      [state, read_handle = std::move(read_handle), write_handle = std::move(write_handle)]() mutable {
                        state->Start(state, std::move(read_handle), std::move(write_handle));
                      });

  if (!done.TimedWait(std::chrono::seconds(30)) || !state->success.load(std::memory_order_acquire)) {
    return {};
  }

  BenchResult result;
  result.chunk_size = chunk_size;
  result.total_bytes = total_bytes;
  result.elapsed_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(state->finished - state->started).count());
  const double seconds = static_cast<double>(result.elapsed_us) / 1'000'000.0;
  result.throughput_mb_s = static_cast<double>(total_bytes) / (1024.0 * 1024.0) / seconds;
  result.chunks_per_second = static_cast<double>((total_bytes + chunk_size - 1) / chunk_size) / seconds;
  return result;
}

void PrintHeader() {
  std::cout << std::left << std::setw(12) << "Chunk" << std::setw(14) << "Total" << std::setw(12) << "Time(ms)"
            << std::setw(14) << "Throughput" << std::setw(16) << "Chunks/s" << std::endl;
  std::cout << std::string(68, '-') << std::endl;
}

void PrintRow(const BenchResult &result) {
  const std::string throughput = (std::to_string(result.throughput_mb_s)).substr(0, 6) + " MB/s";
  const std::string chunks_per_second = std::to_string(static_cast<std::uint64_t>(result.chunks_per_second));
  std::cout << std::left << std::setw(12) << FormatSize(result.chunk_size) << std::setw(14)
            << FormatSize(result.total_bytes) << std::setw(12) << std::fixed << std::setprecision(2)
            << (static_cast<double>(result.elapsed_us) / 1000.0) << std::setw(14) << throughput << std::setw(16)
            << chunks_per_second << std::endl;
}

std::size_t ParseTotalBytes(int argc, char *argv[]) {
  if (argc < 2) {
    return kDefaultTotalBytes;
  }
  const long long total_mb = std::atoll(argv[1]);
  if (total_mb <= 0) {
    return kDefaultTotalBytes;
  }
  return static_cast<std::size_t>(total_mb) * 1024 * 1024;
}

} // namespace

int main(int argc, char *argv[]) {
  nei::AtExitManager at_exit;

  const std::size_t total_bytes = ParseTotalBytes(argc, argv);
  const std::vector<std::size_t> chunk_sizes = {
      4 * 1024,
      16 * 1024,
      64 * 1024,
      256 * 1024,
      1024 * 1024,
  };

  nei::Thread io_thread("pipe-stream-bench-io");
  nei::Thread::Options options;
  options.message_pump_type = nei::MessagePumpType::IO;
  if (!io_thread.StartWithOptions(options)) {
    std::cerr << "Failed to start IO thread." << std::endl;
    return 1;
  }

  const nei::scoped_refptr<nei::TaskRunner> io_runner = io_thread.GetTaskRunner();
  if (!io_runner) {
    std::cerr << "Failed to acquire IO task runner." << std::endl;
    io_thread.Stop();
    return 1;
  }

#if defined(_WIN32)
  std::cout << "=== PipeStream Benchmark (Windows) ===" << std::endl;
#else
  std::cout << "=== PipeStream Benchmark (POSIX) ===" << std::endl;
#endif
  std::cout << "Total bytes per run: " << FormatSize(total_bytes) << std::endl;
  std::cout << std::endl;

  PrintHeader();
  bool all_ok = true;
  for (std::size_t chunk_size : chunk_sizes) {
    BenchResult result = RunBench(io_runner, chunk_size, total_bytes);
    if (result.elapsed_us == 0) {
      std::cerr << "Benchmark failed for chunk=" << chunk_size << std::endl;
      all_ok = false;
      break;
    }
    PrintRow(result);
  }

  io_thread.Stop();
  return all_ok ? 0 : 1;
}
