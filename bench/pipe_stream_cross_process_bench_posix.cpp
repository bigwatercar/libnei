#if !defined(_WIN32)

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include <neixx/io/pipe_stream.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace {

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::duration<double, std::micro>;

constexpr int kDefaultIterations = 5000;
constexpr std::size_t kDefaultPayloadSize = 64;

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

struct Stats {
  double min_us = 0.0;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double avg_us = 0.0;
  double max_us = 0.0;
  double msg_per_sec = 0.0;
};

Stats ComputeStats(const std::vector<double>& samples) {
  Stats stats;
  if (samples.empty()) {
    return stats;
  }
  std::vector<double> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  const auto pick = [&sorted](double q) {
    const std::size_t index = static_cast<std::size_t>(
        q * static_cast<double>(sorted.size() - 1));
    return sorted[index];
  };
  stats.min_us = sorted.front();
  stats.p50_us = pick(0.50);
  stats.p95_us = pick(0.95);
  stats.max_us = sorted.back();
  stats.avg_us = std::accumulate(sorted.begin(), sorted.end(), 0.0) /
                 static_cast<double>(sorted.size());
  return stats;
}

void PrintStats(std::size_t payload_size,
                int iterations,
                const Stats& stats) {
  std::cout << std::left << std::setw(12) << "Payload"
            << std::setw(12) << "Iters"
            << std::setw(12) << "Min(us)"
            << std::setw(12) << "P50(us)"
            << std::setw(12) << "P95(us)"
            << std::setw(12) << "Avg(us)"
            << std::setw(12) << "Max(us)"
            << std::setw(14) << "Msgs/s" << std::endl;
  std::cout << std::string(86, '-') << std::endl;
  std::cout << std::left << std::setw(12) << payload_size
            << std::setw(12) << iterations
            << std::setw(12) << std::fixed << std::setprecision(2)
            << stats.min_us
            << std::setw(12) << stats.p50_us
            << std::setw(12) << stats.p95_us
            << std::setw(12) << stats.avg_us
            << std::setw(12) << stats.max_us
            << std::setw(14) << std::fixed << std::setprecision(0)
            << stats.msg_per_sec << std::endl;
}

bool ReadExactFd(int fd, char* data, std::size_t bytes) {
  std::size_t offset = 0;
  while (offset < bytes) {
    const ssize_t n = read(fd, data + offset, bytes - offset);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(n);
  }
  return true;
}

bool WriteExactFd(int fd, const char* data, std::size_t bytes) {
  std::size_t offset = 0;
  while (offset < bytes) {
    const ssize_t n = write(fd, data + offset, bytes - offset);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(n);
  }
  return true;
}

bool RunChild(int read_fd, int write_fd, std::size_t payload_size, int iterations) {
  std::vector<char> buffer(payload_size);
  for (int i = 0; i < iterations; ++i) {
    if (!ReadExactFd(read_fd, buffer.data(), payload_size)) {
      close(read_fd);
      close(write_fd);
      return false;
    }
    if (!WriteExactFd(write_fd, buffer.data(), payload_size)) {
      close(read_fd);
      close(write_fd);
      return false;
    }
  }
  close(read_fd);
  close(write_fd);
  return true;
}

struct BenchLoop : public std::enable_shared_from_this<BenchLoop> {
  std::shared_ptr<nei::PipeInputStream> input;
  std::shared_ptr<nei::PipeOutputStream> output;
  nei::scoped_refptr<nei::IOBufferWithSize> payload;
  nei::scoped_refptr<nei::IOBuffer> payload_view;
  BufferHolder read_holder;
  std::size_t payload_size = 0;
  int iterations = 0;
  int current = 0;
  Clock::time_point start_time;
  std::vector<double>* latencies = nullptr;
  nei::WaitableEvent* done = nullptr;
  std::atomic<bool>* ok = nullptr;

  void Kick() {
    if (current >= iterations) {
      ok->store(true, std::memory_order_release);
      done->Signal();
      return;
    }
    start_time = Clock::now();
    input->ReadAsync(
        read_holder.buf, payload_size,
        [self = shared_from_this()](bool read_ok, std::size_t n) {
          if (!read_ok || n != self->payload_size) {
            self->done->Signal();
            return;
          }
          self->latencies->push_back(
              std::chrono::duration_cast<Microseconds>(Clock::now() - self->start_time)
                  .count());
          ++self->current;
          self->Kick();
        });
    output->WriteAsync(
        payload_view, payload_size,
        [self = shared_from_this()](bool write_ok, std::size_t n) {
          if (!write_ok || n != self->payload_size) {
            self->done->Signal();
          }
        });
  }
};

bool RunParent(std::size_t payload_size, int iterations, Stats* stats_out) {
  int parent_to_child[2] = {-1, -1};
  int child_to_parent[2] = {-1, -1};
  if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(parent_to_child[0]);
    close(parent_to_child[1]);
    close(child_to_parent[0]);
    close(child_to_parent[1]);
    return false;
  }

  if (pid == 0) {
    close(parent_to_child[1]);
    close(child_to_parent[0]);
    const bool ok = RunChild(parent_to_child[0], child_to_parent[1],
                             payload_size, iterations);
    _exit(ok ? 0 : 1);
  }

  close(parent_to_child[0]);
  close(child_to_parent[1]);

  auto* io_thread = new nei::Thread("pipe-stream-xproc-bench-parent-io");
  nei::Thread::Options options;
  options.message_pump_type = nei::MessagePumpType::IO;
  if (!io_thread->StartWithOptions(options)) {
    close(parent_to_child[1]);
    close(child_to_parent[0]);
    waitpid(pid, nullptr, 0);
    delete io_thread;
    return false;
  }

  auto io_runner = io_thread->GetTaskRunner();
  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};
  std::vector<double> latencies_us;
  latencies_us.reserve(static_cast<std::size_t>(iterations));

  io_runner->PostTask(FROM_HERE, [io_runner, &done, &ok, &latencies_us,
                                  write_fd = parent_to_child[1],
                                  read_fd = child_to_parent[0],
                                  payload_size, iterations]() {
    auto input = std::make_shared<nei::PipeInputStream>(io_runner);
    auto output = std::make_shared<nei::PipeOutputStream>(io_runner);
    if (!output->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle(write_fd)) ||
        !input->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle(read_fd))) {
      done.Signal();
      return;
    }

    auto payload = AcquireBuffer(payload_size);
    for (std::size_t i = 0; i < payload_size; ++i) {
      payload.buf->data()[i] = static_cast<char>((i * 17u + 3u) & 0xFFu);
    }

    auto loop = std::make_shared<BenchLoop>();
    loop->input = input;
    loop->output = output;
    loop->payload = payload.sized;
    loop->payload_view = nei::scoped_refptr<nei::IOBuffer>(payload.sized.get());
    loop->read_holder = AcquireBuffer(payload_size);
    loop->payload_size = payload_size;
    loop->iterations = iterations;
    loop->latencies = &latencies_us;
    loop->done = &done;
    loop->ok = &ok;
    loop->Kick();
  });

  const bool finished = done.TimedWait(std::chrono::seconds(60)) &&
                        ok.load(std::memory_order_acquire);

  int status = 0;
  const pid_t waited = waitpid(pid, &status, 0);
  (void)io_thread;

  if (!finished || waited != pid ||
      !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return false;
  }

  *stats_out = ComputeStats(latencies_us);
  stats_out->msg_per_sec =
      1'000'000.0 / (stats_out->avg_us > 0.0 ? stats_out->avg_us : 1.0);
  return true;
}

std::size_t ParsePayloadSize(int argc, char* argv[]) {
  if (argc >= 2) {
    const long long payload = std::atoll(argv[1]);
    if (payload > 0) {
      return static_cast<std::size_t>(payload);
    }
  }
  return kDefaultPayloadSize;
}

int ParseIterations(int argc, char* argv[]) {
  if (argc >= 3) {
    const int iters = std::atoi(argv[2]);
    if (iters > 0) {
      return iters;
    }
  }
  return kDefaultIterations;
}

}  // namespace

int main(int argc, char* argv[]) {
  nei::AtExitManager at_exit;

  const std::size_t payload_size = ParsePayloadSize(argc, argv);
  const int iterations = ParseIterations(argc, argv);

  std::cout << "=== PipeStream Cross-Process Ping-Pong Benchmark (POSIX) ==="
            << std::endl;
  std::cout << "Payload: " << payload_size << " bytes, iterations: "
            << iterations << std::endl << std::endl;

  Stats stats;
  if (!RunParent(payload_size, iterations, &stats)) {
    std::cerr << "PipeStream cross-process benchmark FAILED." << std::endl;
    return 1;
  }

  PrintStats(payload_size, iterations, stats);
  std::cout.flush();
  std::cerr.flush();
  std::_Exit(0);
}

#endif  // !defined(_WIN32)
