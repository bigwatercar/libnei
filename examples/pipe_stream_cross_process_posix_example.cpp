#if !defined(_WIN32)

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <neixx/common/location.h>
#include <neixx/common/platform_handle.h>
#include <neixx/common/at_exit.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/io_thread.h>
#include <neixx/io/pipe_stream.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>

namespace {

constexpr std::size_t kBufferSize = 4096;

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

bool RunChild(int read_fd, int write_fd) {
  nei::IOThread::Start();
  const nei::scoped_refptr<nei::SingleThreadTaskRunner> io_runner = nei::GetGlobalIOTaskRunner();
  if (!io_runner) {
    std::cerr << "[child] Failed to start IO thread." << std::endl;
    close(read_fd);
    close(write_fd);
    return false;
  }
  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  io_runner->PostTask(FROM_HERE, [io_runner, &done, &ok, read_fd, write_fd]() mutable {
    auto input = std::make_shared<nei::PipeInputStream>(io_runner);
    auto output = std::make_shared<nei::PipeOutputStream>(io_runner);
    if (!input->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle(read_fd))
        || !output->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle(write_fd))) {
      done.Signal();
      return;
    }

    auto read_holder = AcquireBuffer(kBufferSize);
    input->ReadAsync(
        read_holder.buf, kBufferSize, [io_runner, &done, &ok, read_holder, input, output](bool success, std::size_t n) {
          if (!success || n == 0) {
            done.Signal();
            return;
          }

          const std::string request(reinterpret_cast<const char *>(read_holder.buf->data()), n);
          std::cout << "[child] received: " << request << std::endl;
          if (request != "ping from parent") {
            done.Signal();
            return;
          }

          const std::string reply = "pong from child";
          auto write_holder = AcquireBuffer(reply.size());
          std::memcpy(write_holder.buf->data(), reply.data(), reply.size());
          output->WriteAsync(write_holder.buf,
                             reply.size(),
                             [&done, &ok, write_holder, output](bool write_success, std::size_t written) {
                               ok.store(write_success && written == 15, std::memory_order_release);
                               done.Signal();
                             });
        });
  });

  const bool finished = done.TimedWait(std::chrono::seconds(10));
  return finished && ok.load(std::memory_order_acquire);
}

bool RunParent(pid_t child_pid, int write_fd, int read_fd) {
  nei::IOThread::Start();
  const nei::scoped_refptr<nei::SingleThreadTaskRunner> io_runner = nei::GetGlobalIOTaskRunner();
  if (!io_runner) {
    std::cerr << "[parent] Failed to start IO thread." << std::endl;
    close(write_fd);
    close(read_fd);
    return false;
  }
  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::string response;
  std::atomic<bool> ok{false};

  io_runner->PostTask(FROM_HERE, [io_runner, &done, &ok, &response, write_fd, read_fd]() mutable {
    auto output = std::make_shared<nei::PipeOutputStream>(io_runner);
    auto input = std::make_shared<nei::PipeInputStream>(io_runner);
    if (!output->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle(write_fd))
        || !input->BindPlatformHandle(nei::PlatformHandle::FromNativeHandle(read_fd))) {
      done.Signal();
      return;
    }

    auto read_holder = AcquireBuffer(kBufferSize);
    input->ReadAsync(
        read_holder.buf, kBufferSize, [&done, &ok, &response, read_holder, input](bool success, std::size_t n) {
          if (success && n > 0) {
            response.assign(reinterpret_cast<const char *>(read_holder.buf->data()), n);
            ok.store(response == "pong from child", std::memory_order_release);
          }
          done.Signal();
        });

    const std::string request = "ping from parent";
    auto write_holder = AcquireBuffer(request.size());
    std::memcpy(write_holder.buf->data(), request.data(), request.size());
    output->WriteAsync(write_holder.buf, request.size(), [write_holder, output](bool, std::size_t) {});
  });

  const bool finished = done.TimedWait(std::chrono::seconds(10));
  int status = 0;
  const pid_t waited = waitpid(child_pid, &status, 0);

  if (!finished || !ok.load(std::memory_order_acquire) || waited != child_pid || !WIFEXITED(status)
      || WEXITSTATUS(status) != 0) {
    std::cerr << "[parent] Demo failed, response='" << response << "'" << std::endl;
    return false;
  }

  std::cout << "[parent] received: " << response << std::endl;
  return true;
}

} // namespace

int main() {
  nei::AtExitManager at_exit;

  int parent_to_child[2] = {-1, -1};
  int child_to_parent[2] = {-1, -1};
  if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
    std::cerr << "Failed to create POSIX pipes." << std::endl;
    return 1;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "fork() failed." << std::endl;
    close(parent_to_child[0]);
    close(parent_to_child[1]);
    close(child_to_parent[0]);
    close(child_to_parent[1]);
    return 1;
  }

  if (pid == 0) {
    close(parent_to_child[1]);
    close(child_to_parent[0]);
    const bool ok = RunChild(parent_to_child[0], child_to_parent[1]);
    return ok ? 0 : 1;
  }

  close(parent_to_child[0]);
  close(child_to_parent[1]);
  const bool ok = RunParent(pid, parent_to_child[1], child_to_parent[0]);
  if (!ok) {
    std::cerr << "PipeStream POSIX cross-process demo FAILED." << std::endl;
    return 1;
  }

  std::cout << "PipeStream POSIX cross-process demo completed successfully." << std::endl;
  return 0;
}

#endif // !defined(_WIN32)
