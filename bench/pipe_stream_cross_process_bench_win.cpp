#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <neixx/common/at_exit.h>
#include <neixx/common/location.h>
#include <neixx/common/platform_handle.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/io_thread.h>
#include <neixx/io/pipe_stream.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>

namespace {

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::duration<double, std::micro>;

constexpr int kDefaultIterations = 5000;
constexpr std::size_t kDefaultPayloadSize = 64;

struct BufferHolder {
  nei::scoped_refptr<nei::PooledIOBuffer> sized;
  nei::scoped_refptr<nei::IOBuffer> buf;
};

BufferHolder AcquireBuffer(std::size_t size) {
  BufferHolder holder;
  holder.sized = nei::IOBufferPool::GetInstance().AcquireBuffer(size);
  holder.buf = nei::scoped_refptr<nei::IOBuffer>(holder.sized.get());
  return holder;
}

std::string BuildPipeName(const char *suffix) {
  static std::atomic<unsigned long long> counter{0};
  const unsigned long long id = counter.fetch_add(1, std::memory_order_relaxed);
  return "\\\\.\\pipe\\nei_pipe_stream_xproc_bench_" + std::to_string(GetCurrentProcessId()) + "_"
         + std::to_string(GetTickCount64()) + "_" + std::to_string(id) + "_" + suffix;
}

bool ConnectNamedPipeServer(HANDLE pipe) {
  const BOOL ok = ConnectNamedPipe(pipe, nullptr);
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  return ok || error == ERROR_PIPE_CONNECTED;
}

std::string GetSelfPath() {
  char buffer[MAX_PATH] = {};
  const DWORD written = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (written == 0 || written >= MAX_PATH) {
    return std::string();
  }
  return std::string(buffer, written);
}

struct Stats {
  double min_us = 0.0;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double avg_us = 0.0;
  double max_us = 0.0;
  double msg_per_sec = 0.0;
};

Stats ComputeStats(const std::vector<double> &samples) {
  Stats stats;
  if (samples.empty()) {
    return stats;
  }
  std::vector<double> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  const auto pick = [&sorted](double q) {
    const std::size_t index = static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1));
    return sorted[index];
  };
  stats.min_us = sorted.front();
  stats.p50_us = pick(0.50);
  stats.p95_us = pick(0.95);
  stats.max_us = sorted.back();
  stats.avg_us = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
  return stats;
}

void PrintStats(std::size_t payload_size, int iterations, const Stats &stats) {
  std::cout << std::left << std::setw(12) << "Payload" << std::setw(12) << "Iters" << std::setw(12) << "Min(us)"
            << std::setw(12) << "P50(us)" << std::setw(12) << "P95(us)" << std::setw(12) << "Avg(us)" << std::setw(12)
            << "Max(us)" << std::setw(14) << "Msgs/s" << std::endl;
  std::cout << std::string(86, '-') << std::endl;
  std::cout << std::left << std::setw(12) << payload_size << std::setw(12) << iterations << std::setw(12) << std::fixed
            << std::setprecision(2) << stats.min_us << std::setw(12) << stats.p50_us << std::setw(12) << stats.p95_us
            << std::setw(12) << stats.avg_us << std::setw(12) << stats.max_us << std::setw(14) << std::fixed
            << std::setprecision(0) << stats.msg_per_sec << std::endl;
}

using ReadExactCallback = std::function<void(bool, BufferHolder)>;
using WriteExactCallback = std::function<void(bool)>;

struct ReadExactState : public std::enable_shared_from_this<ReadExactState> {
  std::shared_ptr<nei::PipeInputStream> stream;
  BufferHolder holder;
  std::size_t total = 0;
  std::size_t offset = 0;
  ReadExactCallback callback;

  void Issue() {
    auto slice = nei::scoped_refptr<nei::IOBuffer>(new nei::WrappedIOBuffer(holder.buf->data() + offset));
    stream->ReadAsync(slice, total - offset, [self = shared_from_this(), slice](bool ok, std::size_t n) {
      if (!ok || n == 0) {
        self->callback(false, self->holder);
        return;
      }
      self->offset += n;
      if (self->offset >= self->total) {
        self->callback(true, self->holder);
        return;
      }
      self->Issue();
    });
  }
};

void ReadExact(const std::shared_ptr<nei::PipeInputStream> &stream, std::size_t bytes, ReadExactCallback callback) {
  auto state = std::make_shared<ReadExactState>();
  state->stream = stream;
  state->holder = AcquireBuffer(bytes);
  state->total = bytes;
  state->callback = std::move(callback);
  state->Issue();
}

struct WriteExactState : public std::enable_shared_from_this<WriteExactState> {
  std::shared_ptr<nei::PipeOutputStream> stream;
  nei::scoped_refptr<nei::PooledIOBuffer> base;
  std::size_t total = 0;
  std::size_t offset = 0;
  WriteExactCallback callback;

  void Issue() {
    auto slice = nei::scoped_refptr<nei::IOBuffer>(new nei::WrappedIOBuffer(base->data() + offset));
    stream->WriteAsync(slice, total - offset, [self = shared_from_this(), slice](bool ok, std::size_t n) {
      if (!ok || n == 0) {
        self->callback(false);
        return;
      }
      self->offset += n;
      if (self->offset >= self->total) {
        self->callback(true);
        return;
      }
      self->Issue();
    });
  }
};

void WriteExact(const std::shared_ptr<nei::PipeOutputStream> &stream,
                const nei::scoped_refptr<nei::PooledIOBuffer> &buffer,
                std::size_t bytes,
                WriteExactCallback callback) {
  auto state = std::make_shared<WriteExactState>();
  state->stream = stream;
  state->base = buffer;
  state->total = bytes;
  state->callback = std::move(callback);
  state->Issue();
}

struct ChildLoopState : public std::enable_shared_from_this<ChildLoopState> {
  std::shared_ptr<nei::PipeInputStream> input;
  std::shared_ptr<nei::PipeOutputStream> output;
  std::size_t payload_size = 0;
  int iterations = 0;
  int current = 0;
  nei::WaitableEvent *done = nullptr;
  std::atomic<bool> *ok = nullptr;

  void RunNext() {
    if (current >= iterations) {
      ok->store(true, std::memory_order_release);
      done->Signal();
      return;
    }
    ReadExact(input, payload_size, [self = shared_from_this()](bool read_ok, BufferHolder holder) {
      if (!read_ok) {
        self->done->Signal();
        return;
      }
      WriteExact(self->output, holder.sized, self->payload_size, [self](bool write_ok) {
        if (!write_ok) {
          self->done->Signal();
          return;
        }
        ++self->current;
        self->RunNext();
      });
    });
  }
};

bool RunChild(const std::string &read_pipe_name,
              const std::string &write_pipe_name,
              std::size_t payload_size,
              int iterations) {
  HANDLE read_handle = CreateFileA(read_pipe_name.c_str(),
                                   GENERIC_READ,
                                   0,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                   nullptr);
  if (read_handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  HANDLE write_handle = CreateFileA(write_pipe_name.c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                    nullptr);
  if (write_handle == INVALID_HANDLE_VALUE) {
    CloseHandle(read_handle);
    return false;
  }

  nei::IOThread::Start();
  auto io_runner = nei::GetGlobalIOTaskRunner();
  if (!io_runner) {
    CloseHandle(read_handle);
    CloseHandle(write_handle);
    return false;
  }

  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};
  io_runner->PostTask(FROM_HERE, [io_runner, &done, &ok, read_handle, write_handle, payload_size, iterations]() {
    auto input = std::make_shared<nei::PipeInputStream>(io_runner);
    auto output = std::make_shared<nei::PipeOutputStream>(io_runner);
    if (!input->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(read_handle))
        || !output->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(write_handle))) {
      done.Signal();
      return;
    }

    auto state = std::make_shared<ChildLoopState>();
    state->input = input;
    state->output = output;
    state->payload_size = payload_size;
    state->iterations = iterations;
    state->done = &done;
    state->ok = &ok;
    state->RunNext();
  });

  const bool finished = done.TimedWait(std::chrono::seconds(60));
  return finished && ok.load(std::memory_order_acquire);
}

struct ParentLoopState : public std::enable_shared_from_this<ParentLoopState> {
  std::shared_ptr<nei::PipeInputStream> input;
  std::shared_ptr<nei::PipeOutputStream> output;
  nei::scoped_refptr<nei::PooledIOBuffer> payload;
  std::size_t payload_size = 0;
  int iterations = 0;
  int current = 0;
  Clock::time_point start_time;
  std::vector<double> latencies_us;
  nei::WaitableEvent *done = nullptr;
  std::atomic<bool> *ok = nullptr;

  void Kick() {
    if (current >= iterations) {
      ok->store(true, std::memory_order_release);
      done->Signal();
      return;
    }

    ReadExact(input, payload_size, [self = shared_from_this()](bool read_ok, BufferHolder /*holder*/) {
      if (!read_ok) {
        self->done->Signal();
        return;
      }
      const auto now = Clock::now();
      self->latencies_us.push_back(std::chrono::duration_cast<Microseconds>(now - self->start_time).count());
      ++self->current;
      self->Kick();
    });

    start_time = Clock::now();
    WriteExact(output, payload, payload_size, [self = shared_from_this()](bool write_ok) {
      if (!write_ok) {
        self->done->Signal();
      }
    });
  }
};

bool RunParent(std::size_t payload_size, int iterations, Stats *stats_out) {
  const std::string parent_to_child = BuildPipeName("p2c");
  const std::string child_to_parent = BuildPipeName("c2p");

  HANDLE write_server = CreateNamedPipeA(parent_to_child.c_str(),
                                         PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                         1,
                                         0,
                                         0,
                                         0,
                                         nullptr);
  if (write_server == INVALID_HANDLE_VALUE) {
    return false;
  }
  HANDLE read_server = CreateNamedPipeA(child_to_parent.c_str(),
                                        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                        1,
                                        0,
                                        0,
                                        0,
                                        nullptr);
  if (read_server == INVALID_HANDLE_VALUE) {
    CloseHandle(write_server);
    return false;
  }

  const std::string self_path = GetSelfPath();
  if (self_path.empty()) {
    CloseHandle(write_server);
    CloseHandle(read_server);
    return false;
  }

  std::string command_line = "\"" + self_path + "\" --child \"" + parent_to_child + "\" \"" + child_to_parent + "\" "
                             + std::to_string(payload_size) + " " + std::to_string(iterations);
  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(write_server);
    CloseHandle(read_server);
    return false;
  }

  const bool connected_write = ConnectNamedPipeServer(write_server);
  const bool connected_read = ConnectNamedPipeServer(read_server);
  if (!connected_write || !connected_read) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(write_server);
    CloseHandle(read_server);
    return false;
  }

  nei::IOThread::Start();
  auto io_runner = nei::GetGlobalIOTaskRunner();
  if (!io_runner) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(write_server);
    CloseHandle(read_server);
    return false;
  }
  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};
  std::shared_ptr<ParentLoopState> state = std::make_shared<ParentLoopState>();
  io_runner->PostTask(FROM_HERE, [io_runner, &done, &ok, state, write_server, read_server, payload_size, iterations]() {
    auto input = std::make_shared<nei::PipeInputStream>(io_runner);
    auto output = std::make_shared<nei::PipeOutputStream>(io_runner);
    if (!output->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(write_server))
        || !input->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(read_server))) {
      done.Signal();
      return;
    }

    auto payload = AcquireBuffer(payload_size);
    for (std::size_t i = 0; i < payload_size; ++i) {
      payload.buf->data()[i] = static_cast<unsigned char>((i * 17u + 3u) & 0xFFu);
    }

    state->input = input;
    state->output = output;
    state->payload = payload.sized;
    state->payload_size = payload_size;
    state->iterations = iterations;
    state->latencies_us.reserve(static_cast<std::size_t>(iterations));
    state->done = &done;
    state->ok = &ok;
    state->Kick();
  });

  const bool finished = done.TimedWait(std::chrono::seconds(60));
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  if (!finished || !ok.load(std::memory_order_acquire) || exit_code != 0) {
    return false;
  }

  *stats_out = ComputeStats(state->latencies_us);
  stats_out->msg_per_sec = 1'000'000.0 / (stats_out->avg_us > 0.0 ? stats_out->avg_us : 1.0);
  return true;
}

std::size_t ParsePayloadSize(int argc, char *argv[]) {
  if (argc >= 2) {
    const long long payload = std::atoll(argv[1]);
    if (payload > 0) {
      return static_cast<std::size_t>(payload);
    }
  }
  return kDefaultPayloadSize;
}

int ParseIterations(int argc, char *argv[]) {
  if (argc >= 3) {
    const int iters = std::atoi(argv[2]);
    if (iters > 0) {
      return iters;
    }
  }
  return kDefaultIterations;
}

} // namespace

int main(int argc, char *argv[]) {
  nei::AtExitManager at_exit;

  if (argc == 6 && std::string(argv[1]) == "--child") {
    return RunChild(argv[2], argv[3], static_cast<std::size_t>(std::atoll(argv[4])), std::atoi(argv[5])) ? 0 : 1;
  }

  const std::size_t payload_size = ParsePayloadSize(argc, argv);
  const int iterations = ParseIterations(argc, argv);

  std::cout << "=== PipeStream Cross-Process Ping-Pong Benchmark (Windows) ===" << std::endl;
  std::cout << "Payload: " << payload_size << " bytes, iterations: " << iterations << std::endl << std::endl;

  Stats stats;
  if (!RunParent(payload_size, iterations, &stats)) {
    std::cerr << "PipeStream cross-process benchmark FAILED." << std::endl;
    return 1;
  }

  PrintStats(payload_size, iterations, stats);
  return 0;
}

#endif // defined(_WIN32)
