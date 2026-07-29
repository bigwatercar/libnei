#if defined(_WIN32)

#include <windows.h>

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
#include <neixx/io/pipe_stream.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace {

constexpr std::size_t kBufferSize = 4096;

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

std::string BuildPipeName(const char* suffix) {
  static std::atomic<unsigned long long> counter{0};
  const unsigned long long id =
      counter.fetch_add(1, std::memory_order_relaxed);
  return "\\\\.\\pipe\\nei_pipe_stream_demo_" +
         std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(GetTickCount64()) + "_" + std::to_string(id) + "_" +
         suffix;
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

bool RunChild(const std::string& read_pipe_name,
              const std::string& write_pipe_name) {
  HANDLE read_handle = CreateFileA(
      read_pipe_name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (read_handle == INVALID_HANDLE_VALUE) {
    std::cerr << "[child] Failed to open read pipe, error=" << GetLastError()
              << std::endl;
    return false;
  }

  HANDLE write_handle = CreateFileA(
      write_pipe_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (write_handle == INVALID_HANDLE_VALUE) {
    std::cerr << "[child] Failed to open write pipe, error=" << GetLastError()
              << std::endl;
    CloseHandle(read_handle);
    return false;
  }

  nei::Thread io_thread("pipe-stream-win-demo-child-io");
  nei::Thread::Options options;
  options.message_pump_type = nei::MessagePumpType::IO;
  if (!io_thread.StartWithOptions(options)) {
    std::cerr << "[child] Failed to start IO thread." << std::endl;
    CloseHandle(read_handle);
    CloseHandle(write_handle);
    return false;
  }

  const nei::scoped_refptr<nei::TaskRunner> io_runner = io_thread.GetTaskRunner();
  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};

  io_runner->PostTask(
      FROM_HERE,
      [io_runner, &done, &ok, read_handle, write_handle]() mutable {
        auto input = std::make_shared<nei::PipeInputStream>(io_runner);
        auto output = std::make_shared<nei::PipeOutputStream>(io_runner);
        if (!input->BindPlatformHandle(
                nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(
                    read_handle)) ||
            !output->BindPlatformHandle(
                nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(
                    write_handle))) {
          done.Signal();
          return;
        }

        auto read_holder = AcquireBuffer(kBufferSize);
        input->ReadAsync(
            read_holder.buf, kBufferSize,
            [io_runner, &done, &ok, read_holder, input, output](bool success,
                                                                 std::size_t n) {
              if (!success || n == 0) {
                done.Signal();
                return;
              }

              const std::string request(reinterpret_cast<const char*>(read_holder.buf->data()), n);
              std::cout << "[child] received: " << request << std::endl;
              if (request != "ping from parent") {
                done.Signal();
                return;
              }

              const std::string reply = "pong from child";
              auto write_holder = AcquireBuffer(reply.size());
              std::memcpy(write_holder.buf->data(), reply.data(), reply.size());
              output->WriteAsync(
                  write_holder.buf, reply.size(),
                  [&done, &ok, write_holder, output](bool write_success,
                                                     std::size_t written) {
                    ok.store(write_success && written == 15,
                             std::memory_order_release);
                    done.Signal();
                  });
            });
      });

  const bool finished = done.TimedWait(std::chrono::seconds(10));
  io_thread.Stop();
  return finished && ok.load(std::memory_order_acquire);
}

bool RunParent() {
  const std::string parent_to_child = BuildPipeName("parent_to_child");
  const std::string child_to_parent = BuildPipeName("child_to_parent");

  HANDLE write_server = CreateNamedPipeA(
      parent_to_child.c_str(),
      PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 0, 0, nullptr);
  if (write_server == INVALID_HANDLE_VALUE) {
    std::cerr << "[parent] Failed to create write server pipe, error="
              << GetLastError() << std::endl;
    return false;
  }

  HANDLE read_server = CreateNamedPipeA(
      child_to_parent.c_str(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 0, 0, nullptr);
  if (read_server == INVALID_HANDLE_VALUE) {
    std::cerr << "[parent] Failed to create read server pipe, error="
              << GetLastError() << std::endl;
    CloseHandle(write_server);
    return false;
  }

  const std::string self_path = GetSelfPath();
  if (self_path.empty()) {
    std::cerr << "[parent] Failed to resolve executable path." << std::endl;
    CloseHandle(write_server);
    CloseHandle(read_server);
    return false;
  }

  std::string command_line = "\"" + self_path + "\" --child \"" +
                             parent_to_child + "\" \"" + child_to_parent +
                             "\"";
  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &si, &pi)) {
    std::cerr << "[parent] Failed to spawn child, error=" << GetLastError()
              << std::endl;
    CloseHandle(write_server);
    CloseHandle(read_server);
    return false;
  }

  const bool connected_write = ConnectNamedPipeServer(write_server);
  const bool connected_read = ConnectNamedPipeServer(read_server);
  if (!connected_write || !connected_read) {
    std::cerr << "[parent] Failed to connect named pipes." << std::endl;
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(write_server);
    CloseHandle(read_server);
    return false;
  }

  nei::Thread io_thread("pipe-stream-win-demo-parent-io");
  nei::Thread::Options options;
  options.message_pump_type = nei::MessagePumpType::IO;
  if (!io_thread.StartWithOptions(options)) {
    std::cerr << "[parent] Failed to start IO thread." << std::endl;
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(write_server);
    CloseHandle(read_server);
    return false;
  }

  const nei::scoped_refptr<nei::TaskRunner> io_runner = io_thread.GetTaskRunner();
  nei::WaitableEvent done(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::string response;
  std::atomic<bool> ok{false};

  io_runner->PostTask(
      FROM_HERE,
      [io_runner, &done, &ok, &response, write_server, read_server]() mutable {
        auto output = std::make_shared<nei::PipeOutputStream>(io_runner);
        auto input = std::make_shared<nei::PipeInputStream>(io_runner);
        if (!output->BindPlatformHandle(
                nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(
                    write_server)) ||
            !input->BindPlatformHandle(
                nei::PlatformHandle::FromNativeHandle<nei::DefaultHandleTraits>(
                    read_server))) {
          done.Signal();
          return;
        }

        auto read_holder = AcquireBuffer(kBufferSize);
        input->ReadAsync(
            read_holder.buf, kBufferSize,
            [&done, &ok, &response, read_holder, input](bool success,
                                                        std::size_t n) {
              if (success && n > 0) {
                response.assign(reinterpret_cast<const char*>(read_holder.buf->data()), n);
                ok.store(response == "pong from child",
                         std::memory_order_release);
              }
              done.Signal();
            });

        const std::string request = "ping from parent";
        auto write_holder = AcquireBuffer(request.size());
        std::memcpy(write_holder.buf->data(), request.data(), request.size());
        output->WriteAsync(write_holder.buf, request.size(),
                           [write_holder, output](bool, std::size_t) {});
      });

  const bool finished = done.TimedWait(std::chrono::seconds(10));
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  io_thread.Stop();

  if (!finished || !ok.load(std::memory_order_acquire) || exit_code != 0) {
    std::cerr << "[parent] Demo failed, child_exit=" << exit_code
              << ", response='" << response << "'" << std::endl;
    return false;
  }

  std::cout << "[parent] received: " << response << std::endl;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  nei::AtExitManager at_exit;

  if (argc == 4 && std::string(argv[1]) == "--child") {
    return RunChild(argv[2], argv[3]) ? 0 : 1;
  }

  const bool ok = RunParent();
  if (!ok) {
    std::cerr << "PipeStream Windows cross-process demo FAILED." << std::endl;
    return 1;
  }

  std::cout << "PipeStream Windows cross-process demo completed successfully."
            << std::endl;
  return 0;
}

#endif  // defined(_WIN32)
