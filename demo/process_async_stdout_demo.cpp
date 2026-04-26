#include <neixx/command_line/command_line.h>
#include <neixx/io/async_handle.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/io_context.h>
#include <neixx/process/launch.h>

#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

int main() {
  nei::IOContext io_context;
  std::thread io_thread([&io_context]() { io_context.Run(); });

#if defined(_WIN32)
  const char *argv[] = {"cmd", "/c", "ping 127.0.0.1 -n 3"};
#else
  const char *argv[] = {"ls", "-la"};
#endif

  nei::CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  nei::LaunchOptions options;
  options.stdout_config.mode = nei::StdioMode::kPipe;
  options.io_context = &io_context;

  nei::Process process = nei::LaunchProcess(command_line, std::move(options));
  if (!process.is_valid()) {
    std::cerr << "failed to launch process" << std::endl;
    io_context.Stop();
    io_thread.join();
    return 1;
  }

  std::unique_ptr<nei::AsyncHandle> stdout_stream = process.TakeStdoutStream();
  if (!stdout_stream) {
    std::cerr << "stdout pipe is unavailable" << std::endl;
    io_context.Stop();
    io_thread.join();
    return 1;
  }

  nei::scoped_refptr<nei::IOBuffer> buffer = nei::MakeRefCounted<nei::IOBuffer>(4096);
  std::promise<void> done;
  std::future<void> done_future = done.get_future();
  std::atomic<bool> done_set{false};

  auto read_loop = std::make_shared<std::function<void()>>();
  *read_loop = [&]() {
    stdout_stream->Read(buffer,
                        buffer->size(),
                        [&, read_loop](int result) {
                          if (result > 0) {
                            std::cout.write(reinterpret_cast<const char *>(buffer->data()), result);
                            std::cout.flush();
                            (*read_loop)();
                            return;
                          }

                          if (!done_set.exchange(true)) {
                            done.set_value();
                          }
                        });
  };

  (*read_loop)();
  done_future.wait();

  const int exit_code = process.Wait();

  io_context.Stop();
  io_thread.join();

  std::cout << "\nchild exit code: " << exit_code << std::endl;
  return 0;
}
