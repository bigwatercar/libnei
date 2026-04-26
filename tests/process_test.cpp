#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <neixx/command_line/command_line.h>
#include <neixx/io/io_buffer.h>
#include <neixx/io/io_context.h>
#include <neixx/process/launch.h>
#include <neixx/task/sequenced_task_runner.h>

namespace nei {
namespace {

constexpr const char *kTestEnvKey = "NEI_PROCESS_TEST_ENV";

class IOContextThread final {
public:
  IOContextThread()
      : thread_([this]() { context_.Run(); }) {
  }

  ~IOContextThread() {
    context_.Stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  IOContext &context() {
    return context_;
  }

private:
  IOContext context_;
  std::thread thread_;
};

CommandLine MakeStdoutEchoCommand(const std::string &text) {
#if defined(_WIN32)
  std::string command = "echo " + text;
  const char *argv[] = {"cmd", "/C", command.c_str()};
  return CommandLine(static_cast<int>(std::size(argv)), argv);
#else
  std::string script = "printf '%s' \"" + text + "\"";
  const char *argv[] = {"/bin/sh", "-c", script.c_str()};
  return CommandLine(static_cast<int>(std::size(argv)), argv);
#endif
}

CommandLine MakeStderrEchoCommand(const std::string &text) {
#if defined(_WIN32)
  std::string command = "echo " + text + " 1>&2";
  const char *argv[] = {"cmd", "/C", command.c_str()};
  return CommandLine(static_cast<int>(std::size(argv)), argv);
#else
  std::string script = "printf '%s' \"" + text + "\" 1>&2";
  const char *argv[] = {"/bin/sh", "-c", script.c_str()};
  return CommandLine(static_cast<int>(std::size(argv)), argv);
#endif
}

CommandLine MakeStdinEchoCommand() {
#if defined(_WIN32)
  const char *argv[] = {"cmd", "/V:ON", "/C", "set /p L= & echo !L!"};
#else
  const char *argv[] = {"/bin/sh", "-c", "cat"};
#endif
  return CommandLine(static_cast<int>(std::size(argv)), argv);
}

CommandLine MakeEnvPrintCommand(const char *key) {
#if defined(_WIN32)
  std::string command = "echo %" + std::string(key) + "%";
  const char *argv[] = {"cmd", "/C", command.c_str()};
  return CommandLine(static_cast<int>(std::size(argv)), argv);
#else
  std::string script = "printf '%s' \"$" + std::string(key) + "\"";
  const char *argv[] = {"/bin/sh", "-c", script.c_str()};
  return CommandLine(static_cast<int>(std::size(argv)), argv);
#endif
}

CommandLine MakeExitWithCodeCommand(int code) {
#if defined(_WIN32)
  std::string command = "exit /b " + std::to_string(code);
  const char *argv[] = {"cmd", "/C", command.c_str()};
  return CommandLine(static_cast<int>(std::size(argv)), argv);
#else
  std::string script = "exit " + std::to_string(code);
  const char *argv[] = {"/bin/sh", "-c", script.c_str()};
  return CommandLine(static_cast<int>(std::size(argv)), argv);
#endif
}


std::string ReadAllFromStream(AsyncHandle *stream) {
  if (!stream) {
    return std::string();
  }

  auto done = std::make_shared<std::promise<std::string>>();
  std::future<std::string> done_future = done->get_future();
  auto done_guard = std::make_shared<std::atomic<bool>>(false);
  auto output = std::make_shared<std::string>();
  auto output_mutex = std::make_shared<std::mutex>();
  scoped_refptr<IOBuffer> buffer = MakeRefCounted<IOBuffer>(4096);

  auto read_loop = std::make_shared<std::function<void()>>();
  *read_loop = [stream, buffer, done_guard, output, output_mutex, read_loop, done]() {
    stream->Read(buffer,
                 buffer->size(),
                 [stream, buffer, done_guard, output, output_mutex, read_loop, done](int result) {
                   if (result > 0) {
                     {
                       std::lock_guard<std::mutex> lock(*output_mutex);
                       output->append(reinterpret_cast<const char *>(buffer->data()),
                                      static_cast<std::size_t>(result));
                     }
                     (*read_loop)();
                     return;
                   }

                   if (!done_guard->exchange(true)) {
                     done->set_value(*output);
                   }
                 });
  };

  (*read_loop)();
  return done_future.get();
}

int WriteAllAndClose(AsyncHandle *stream, const std::string &data) {
  if (!stream) {
    return -1;
  }

  scoped_refptr<IOBuffer> buffer = MakeRefCounted<IOBuffer>(data.size());
  if (!data.empty()) {
    std::copy(data.begin(), data.end(), reinterpret_cast<char *>(buffer->data()));
  }

  auto done = std::make_shared<std::promise<int>>();
  std::future<int> done_future = done->get_future();

  stream->Write(buffer,
                data.size(),
                [done](int result) {
                  done->set_value(result);
                });

  const int result = done_future.get();
  stream->Close();
  return result;
}

void SetCurrentEnv(const char *key, const char *value) {
#if defined(_WIN32)
  _putenv_s(key, value);
#else
  setenv(key, value, 1);
#endif
}

} // namespace

TEST(ProcessTest, StdoutPipeReadsChildOutput) {
  IOContextThread io_thread;

  LaunchOptions options;
  options.io_context = &io_thread.context();
  options.stdout_config.mode = StdioMode::kPipe;

  Process process = LaunchProcess(MakeStdoutEchoCommand("HELLO_NEI"), std::move(options));
  ASSERT_TRUE(process.is_valid());

  std::unique_ptr<AsyncHandle> stdout_stream = process.TakeStdoutStream();
  ASSERT_NE(stdout_stream, nullptr);

  const std::string output = ReadAllFromStream(stdout_stream.get());
  const int exit_code = process.Wait();

  EXPECT_EQ(exit_code, 0);
  EXPECT_NE(output.find("HELLO_NEI"), std::string::npos);
}

TEST(ProcessTest, StderrPipeReadsChildOutput) {
  IOContextThread io_thread;

  LaunchOptions options;
  options.io_context = &io_thread.context();
  options.stderr_config.mode = StdioMode::kPipe;

  Process process = LaunchProcess(MakeStderrEchoCommand("HELLO_ERR"), std::move(options));
  ASSERT_TRUE(process.is_valid());

  std::unique_ptr<AsyncHandle> stderr_stream = process.TakeStderrStream();
  ASSERT_NE(stderr_stream, nullptr);

  const std::string output = ReadAllFromStream(stderr_stream.get());
  const int exit_code = process.Wait();

  EXPECT_EQ(exit_code, 0);
  EXPECT_NE(output.find("HELLO_ERR"), std::string::npos);
}

TEST(ProcessTest, StdinPipeWritesDataToChild) {
  IOContextThread io_thread;

  LaunchOptions options;
  options.io_context = &io_thread.context();
  options.stdin_config.mode = StdioMode::kPipe;
  options.stdout_config.mode = StdioMode::kPipe;

  Process process = LaunchProcess(MakeStdinEchoCommand(), std::move(options));
  ASSERT_TRUE(process.is_valid());

  std::unique_ptr<AsyncHandle> stdin_stream = process.TakeStdinStream();
  std::unique_ptr<AsyncHandle> stdout_stream = process.TakeStdoutStream();
  ASSERT_NE(stdin_stream, nullptr);
  ASSERT_NE(stdout_stream, nullptr);

#if defined(_WIN32)
  const std::string line = "stdin-pipe-test\r\n";
#else
  const std::string line = "stdin-pipe-test\n";
#endif

  const int write_result = WriteAllAndClose(stdin_stream.get(), line);
  const std::string output = ReadAllFromStream(stdout_stream.get());
  const int exit_code = process.Wait();

  EXPECT_GT(write_result, 0);
  EXPECT_EQ(exit_code, 0);
  EXPECT_NE(output.find("stdin-pipe-test"), std::string::npos);
}

TEST(ProcessTest, EnvMapOverridesInheritedVariable) {
  SetCurrentEnv(kTestEnvKey, "parent-value");

  IOContextThread io_thread;

  LaunchOptions options;
  options.io_context = &io_thread.context();
  options.stdout_config.mode = StdioMode::kPipe;
  options.env_map[kTestEnvKey] = "child-override";

  Process process = LaunchProcess(MakeEnvPrintCommand(kTestEnvKey), std::move(options));
  ASSERT_TRUE(process.is_valid());

  std::unique_ptr<AsyncHandle> stdout_stream = process.TakeStdoutStream();
  ASSERT_NE(stdout_stream, nullptr);

  const std::string output = ReadAllFromStream(stdout_stream.get());
  const int exit_code = process.Wait();

  EXPECT_EQ(exit_code, 0);
  EXPECT_NE(output.find("child-override"), std::string::npos);
}

TEST(ProcessTest, TerminationCallbackRunsOnTaskRunner) {
  IOContextThread io_thread;

  LaunchOptions options;
  options.io_context = &io_thread.context();

  Process process = LaunchProcess(MakeExitWithCodeCommand(7), std::move(options));
  ASSERT_TRUE(process.is_valid());

  auto runner = SequencedTaskRunner::Create();
  ASSERT_NE(runner, nullptr);

  auto called = std::make_shared<std::atomic<int>>(0);
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> done_future = done->get_future();

  process.SetTerminationCallback(runner,
                                 BindOnce([called, done]() {
                                   called->fetch_add(1, std::memory_order_relaxed);
                                   done->set_value();
                                 }));

  EXPECT_EQ(done_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const int exit_code = process.Wait();

  EXPECT_EQ(exit_code, 7);
  EXPECT_EQ(called->load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(process.IsRunning());
}

} // namespace nei
