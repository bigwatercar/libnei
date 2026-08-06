#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <neixx/command_line/command_line.h>
#include <neixx/common/location.h>
#include <neixx/io/async_stream.h>
#include <neixx/io/io_buffer.h>
#include <neixx/process/child_process.h>
#include <neixx/process/process_service.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>

namespace nei {
namespace {

class CountingProcessListener final : public ChildProcessListener {
public:
  void OnProcessLaunchSucceeded(int /*pid*/) override {
    launch_succeeded.fetch_add(1, std::memory_order_relaxed);
  }

  void OnProcessLaunchFailed() override {
    launch_failed.fetch_add(1, std::memory_order_relaxed);
  }

  void OnProcessTerminated(const ProcessExitInfo & /*info*/) override {
    terminated.fetch_add(1, std::memory_order_relaxed);
  }

  std::atomic<int> launch_succeeded{0};
  std::atomic<int> launch_failed{0};
  std::atomic<int> terminated{0};
};

class CapturingProcessListener final : public ChildProcessListener {
public:
  explicit CapturingProcessListener(WaitableEvent *done_event)
      : done_event_(done_event) {
  }

  void OnProcessLaunchSucceeded(int pid) override {
    launched_pid.store(pid, std::memory_order_release);
    launch_succeeded.store(true, std::memory_order_release);
  }

  void OnProcessLaunchFailed() override {
    launch_failed.store(true, std::memory_order_release);
    if (done_event_ != nullptr) {
      done_event_->Signal();
    }
  }

  void OnProcessTerminated(const ProcessExitInfo &info) override {
    exit_state.store(info.state, std::memory_order_release);
    exit_code.store(info.exit_code, std::memory_order_release);
    terminated.store(true, std::memory_order_release);
    if (done_event_ != nullptr) {
      done_event_->Signal();
    }
  }

  WaitableEvent *done_event_ = nullptr;
  std::atomic<bool> launch_succeeded{false};
  std::atomic<bool> launch_failed{false};
  std::atomic<bool> terminated{false};
  std::atomic<int> launched_pid{-1};
  std::atomic<ProcessState> exit_state{ProcessState::kNotStarted};
  std::atomic<int> exit_code{-1};
};

struct LaunchTestState {
  scoped_refptr<ProcessService> process_service;
  std::unique_ptr<ChildProcess> process;
  std::unique_ptr<CapturingProcessListener> listener;
  std::atomic<bool> post_completed{false};
  std::atomic<bool> saw_line{false};
  std::atomic<bool> launch_returned{false};
  std::atomic<bool> write_callback_called{false};
  std::atomic<bool> write_success{false};
  std::vector<std::uint8_t> captured_bytes;
  std::string captured_line;
};

std::string NormalizePipeText(const std::string &text) {
  std::string result = text;
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result;
}

std::vector<std::string> SplitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

// ---------------------------------------------------------------------------
// PullReadLoop
//
// Issues repeated ReadAsync() calls on `stream` until EOF/error.  On each
// successful chunk the `on_chunk` functor is called with the bytes.
// All calls happen synchronously on the thread that started the loop
// (same thread the stream is bound to), because FakeAsync / proxy streams
// complete callbacks inline or via PostTask to the same sequence.
// The helper keeps the IOBuffer alive via scoped_refptr across each hop.
// ---------------------------------------------------------------------------
constexpr std::size_t kTestReadBufSize = 4096;

struct PullReadState {
  AsyncInputStream *stream = nullptr;
  std::function<void(const char *, std::size_t)> on_chunk;
  std::function<void()> on_eof;
};

// Forward declaration so the lambda can reference it.
static void IssuePullRead(const std::shared_ptr<PullReadState> &state);

static void IssuePullRead(const std::shared_ptr<PullReadState> &state) {
  scoped_refptr<IOBufferWithSize> sized_buf = IOBufferPool::GetInstance().AcquireBuffer(kTestReadBufSize);
  scoped_refptr<IOBuffer> buf(sized_buf.get());
  state->stream->ReadAsync(buf, kTestReadBufSize, [state, buf](bool ok, std::size_t bytes) mutable {
    if (!ok || bytes == 0) {
      if (state->on_eof)
        state->on_eof();
      return;
    }
    if (state->on_chunk)
      state->on_chunk(reinterpret_cast<const char *>(buf->data()), bytes);
    buf.reset();
    IssuePullRead(state);
  });
}

TEST(ChildProcessTest, LaunchWorksWithoutCurrentIoPump) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  CapturingProcessListener listener(&done);
  const scoped_refptr<ProcessService> service = ProcessService::Create();
  ASSERT_TRUE(service);
  ChildProcess process(service);
  process.SetListener(&listener);

#if defined(_WIN32)
  const char *argv[] = {"cmd", "/d", "/c", "exit /b 0"};
#else
  const char *argv[] = {"/bin/sh", "-c", "exit 0"};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  ProcessLaunchOptions options;
  options.stdin_config.type = StdIOType::NULL_IO;
  options.stdout_config.type = StdIOType::NULL_IO;
  options.stderr_config.type = StdIOType::NULL_IO;

  const bool ok = process.Launch(command_line, options);

  ASSERT_TRUE(ok);
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(listener.launch_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(listener.launch_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(listener.terminated.load(std::memory_order_acquire));
  EXPECT_EQ(listener.exit_state.load(std::memory_order_acquire), ProcessState::kExited);
  EXPECT_EQ(listener.exit_code.load(std::memory_order_acquire), 0);
}

TEST(ChildProcessTest, LaunchFromProcessServiceIoThreadDoesNotDeadlock) {
  const scoped_refptr<ProcessService> service = ProcessService::Create();
  ASSERT_TRUE(service);
  ASSERT_TRUE(service->Start());
  const scoped_refptr<SingleThreadTaskRunner> io_runner = service->GetTaskRunner();
  ASSERT_TRUE(io_runner);

  WaitableEvent launch_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent terminated_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto listener = std::make_shared<CapturingProcessListener>(&terminated_done);
  auto process = std::make_shared<ChildProcess>(service);
  process->SetListener(listener.get());
  std::atomic<bool> launch_ok{false};
  std::atomic<bool> task_executed{false};

  const bool posted = io_runner->PostTask(FROM_HERE, [&]() {
#if defined(_WIN32)
    const char *argv[] = {"cmd", "/d", "/c", "exit /b 0"};
#else
    const char* argv[] = {"/bin/sh", "-c", "exit 0"};
#endif
    CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

    ProcessLaunchOptions options;
    options.stdin_config.type = StdIOType::NULL_IO;
    options.stdout_config.type = StdIOType::NULL_IO;
    options.stderr_config.type = StdIOType::NULL_IO;

    launch_ok.store(process->Launch(command_line, options), std::memory_order_release);
    task_executed.store(true, std::memory_order_release);
    launch_done.Signal();
  });
  ASSERT_TRUE(posted);

  ASSERT_TRUE(launch_done.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(task_executed.load(std::memory_order_acquire));
  ASSERT_TRUE(launch_ok.load(std::memory_order_acquire));

  ASSERT_TRUE(terminated_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(listener->launch_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(listener->launch_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(listener->terminated.load(std::memory_order_acquire));
  EXPECT_EQ(listener->exit_state.load(std::memory_order_acquire), ProcessState::kExited);
  EXPECT_EQ(listener->exit_code.load(std::memory_order_acquire), 0);

  process.reset();
}

TEST(ChildProcessTest, LaunchMultipleProcessesWithSharedProcessService) {
  const scoped_refptr<ProcessService> service = ProcessService::Create();
  ASSERT_TRUE(service);
  EXPECT_FALSE(service->IsRunning());

  WaitableEvent first_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  CapturingProcessListener first_listener(&first_done);
  ChildProcess first_process(service);
  first_process.SetListener(&first_listener);

#if defined(_WIN32)
  const char *first_argv[] = {"cmd", "/d", "/c", "exit /b 0"};
#else
  const char *first_argv[] = {"/bin/sh", "-c", "exit 0"};
#endif
  CommandLine first_command_line(static_cast<int>(sizeof(first_argv) / sizeof(first_argv[0])), first_argv);

  ProcessLaunchOptions first_options;
  first_options.stdin_config.type = StdIOType::NULL_IO;
  first_options.stdout_config.type = StdIOType::NULL_IO;
  first_options.stderr_config.type = StdIOType::NULL_IO;

  ASSERT_TRUE(first_process.Launch(first_command_line, first_options));
  ASSERT_TRUE(first_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(first_listener.launch_succeeded.load(std::memory_order_acquire));
  EXPECT_TRUE(first_listener.terminated.load(std::memory_order_acquire));
  EXPECT_EQ(first_listener.exit_state.load(std::memory_order_acquire), ProcessState::kExited);
  EXPECT_EQ(first_listener.exit_code.load(std::memory_order_acquire), 0);
  EXPECT_TRUE(service->IsRunning());

  WaitableEvent second_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  CapturingProcessListener second_listener(&second_done);
  ChildProcess second_process(service);
  second_process.SetListener(&second_listener);

#if defined(_WIN32)
  const char *second_argv[] = {"cmd", "/d", "/c", "exit /b 0"};
#else
  const char *second_argv[] = {"/bin/sh", "-c", "exit 0"};
#endif
  CommandLine second_command_line(static_cast<int>(sizeof(second_argv) / sizeof(second_argv[0])), second_argv);

  ProcessLaunchOptions second_options;
  second_options.stdin_config.type = StdIOType::NULL_IO;
  second_options.stdout_config.type = StdIOType::NULL_IO;
  second_options.stderr_config.type = StdIOType::NULL_IO;
  ASSERT_TRUE(second_process.Launch(second_command_line, second_options));

  ASSERT_TRUE(second_done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(second_listener.launch_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(second_listener.launch_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(second_listener.terminated.load(std::memory_order_acquire));
  EXPECT_EQ(second_listener.exit_state.load(std::memory_order_acquire), ProcessState::kExited);
  EXPECT_EQ(second_listener.exit_code.load(std::memory_order_acquire), 0);
}

TEST(ChildProcessTest, LaunchWithStdoutPipeReadsLine) {
  WaitableEvent line_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent terminated_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto state = std::make_shared<LaunchTestState>();
  state->process_service = ProcessService::Create();
  ASSERT_TRUE(state->process_service);
  state->process = std::make_unique<ChildProcess>(state->process_service);
  state->listener = std::make_unique<CapturingProcessListener>(&terminated_done);

#if defined(_WIN32)
  const char *argv[] = {"cmd", "/d", "/c", "echo child-out"};
#else
  const char *argv[] = {"/bin/sh", "-c", "printf 'child-out\\n'"};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  ProcessLaunchOptions launch_options;
  launch_options.stdout_config.type = StdIOType::PIPE;
  state->process->SetListener(state->listener.get());
  const bool ok = state->process->Launch(command_line, launch_options);
  state->launch_returned.store(ok, std::memory_order_release);

  if (!ok) {
    terminated_done.Signal();
  } else {
    AsyncInputStream *stdout_stream = state->process->GetStdoutStream();
    if (stdout_stream == nullptr) {
      terminated_done.Signal();
    } else {
      auto pull = std::make_shared<PullReadState>();
      pull->stream = stdout_stream;
      pull->on_chunk = [state, &line_done](const char *data, std::size_t n) {
        state->captured_bytes.insert(state->captured_bytes.end(), data, data + n);
        const std::string text(state->captured_bytes.begin(), state->captured_bytes.end());
        for (const auto &line : SplitLines(text)) {
          if (line == "child-out") {
            state->captured_line = line;
            state->saw_line.store(true, std::memory_order_release);
            line_done.Signal();
            break;
          }
        }
      };
      pull->on_eof = []() {};
      IssuePullRead(pull);

      state->post_completed.store(true, std::memory_order_release);
    }
  }

  const bool line_arrived = line_done.TimedWait(std::chrono::seconds(5));
  if (!line_arrived) {
    const std::string captured(state->captured_bytes.begin(), state->captured_bytes.end());
    ADD_FAILURE() << "Timed out waiting echoed line; captured='" << captured << "'";
  }
  ASSERT_TRUE(line_arrived);
  ASSERT_TRUE(terminated_done.TimedWait(std::chrono::seconds(5)));
  state->process.reset();
  state->process_service.reset();

  EXPECT_TRUE(state->post_completed.load(std::memory_order_acquire)
              || state->launch_returned.load(std::memory_order_acquire) == false);
  EXPECT_TRUE(state->launch_returned.load(std::memory_order_acquire));
  EXPECT_TRUE(state->listener->launch_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(state->listener->launch_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(state->saw_line.load(std::memory_order_acquire));
  EXPECT_EQ(state->captured_line, "child-out");
  if (state->listener->terminated.load(std::memory_order_acquire)) {
    EXPECT_EQ(state->listener->exit_state.load(std::memory_order_acquire), ProcessState::kExited);
    EXPECT_EQ(state->listener->exit_code.load(std::memory_order_acquire), 0);
  }
}

TEST(ChildProcessTest, LaunchWithStdinPipeEchoesToStdout) {
  WaitableEvent line_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent write_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent terminated_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto state = std::make_shared<LaunchTestState>();
  state->process_service = ProcessService::Create();
  ASSERT_TRUE(state->process_service);
  state->process = std::make_unique<ChildProcess>(state->process_service);
  state->listener = std::make_unique<CapturingProcessListener>(&terminated_done);

#if defined(_WIN32)
  const char *argv[] = {"cmd", "/d", "/v:on", "/c", "set /p line=& echo !line!"};
#else
  const char *argv[] = {"/bin/sh", "-c", "IFS= read -r line; printf '%s\\n' \"$line\""};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  ProcessLaunchOptions launch_options;
  launch_options.stdin_config.type = StdIOType::PIPE;
  launch_options.stdout_config.type = StdIOType::PIPE;
  state->process->SetListener(state->listener.get());
  const bool ok = state->process->Launch(command_line, launch_options);
  state->launch_returned.store(ok, std::memory_order_release);
  if (!ok) {
    terminated_done.Signal();
  } else {
    AsyncInputStream *stdout_stream = state->process->GetStdoutStream();
    AsyncOutputStream *stdin_stream = state->process->GetStdinStream();
    if (stdout_stream == nullptr || stdin_stream == nullptr) {
      terminated_done.Signal();
    } else {
      // Start pull-read loop on stdout.
      auto pull = std::make_shared<PullReadState>();
      pull->stream = stdout_stream;
      pull->on_chunk = [state, &line_done](const char *data, std::size_t n) {
        state->captured_bytes.insert(state->captured_bytes.end(), data, data + n);
        const std::string text(state->captured_bytes.begin(), state->captured_bytes.end());
        for (const auto &line : SplitLines(text)) {
          if (line == "echo-through-stdin") {
            state->captured_line = line;
            state->saw_line.store(true, std::memory_order_release);
            line_done.Signal();
            break;
          }
        }
      };
      pull->on_eof = []() {};
      IssuePullRead(pull);

      // Write the stdin payload using new IOBuffer-based WriteAsync.
      const std::string payload = "echo-through-stdin\n";
      scoped_refptr<IOBufferWithSize> wbuf_sized = IOBufferPool::GetInstance().AcquireBuffer(payload.size());
      scoped_refptr<IOBuffer> wbuf(wbuf_sized.get());
      std::memcpy(wbuf->data(), payload.data(), payload.size());
      stdin_stream->WriteAsync(wbuf, payload.size(), [state, &write_done](bool success, std::size_t /*bytes*/) {
        state->write_callback_called.store(true, std::memory_order_release);
        state->write_success.store(success, std::memory_order_release);
        write_done.Signal();
      });

      state->post_completed.store(true, std::memory_order_release);
    }
  }

  ASSERT_TRUE(line_done.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(write_done.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(terminated_done.TimedWait(std::chrono::seconds(5)));
  state->process.reset();
  state->process_service.reset();

  EXPECT_TRUE(state->post_completed.load(std::memory_order_acquire)
              || state->launch_returned.load(std::memory_order_acquire) == false);
  EXPECT_TRUE(state->launch_returned.load(std::memory_order_acquire));
  EXPECT_TRUE(state->listener->launch_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(state->listener->launch_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(state->write_callback_called.load(std::memory_order_acquire));
  EXPECT_TRUE(state->saw_line.load(std::memory_order_acquire));
  EXPECT_EQ(state->captured_line, "echo-through-stdin");
}

TEST(ChildProcessTest, TerminateForceTrueKillsRunningProcess) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  CapturingProcessListener listener(&done);
  const scoped_refptr<ProcessService> service = ProcessService::Create();
  ASSERT_TRUE(service);
  ChildProcess process(service);
  process.SetListener(&listener);

#if defined(_WIN32)
  const char *argv[] = {"cmd", "/d", "/c", "ping -n 60 127.0.0.1 > nul"};
#else
  const char *argv[] = {"/bin/sh", "-c", "while :; do sleep 1; done"};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  ProcessLaunchOptions options;
  options.stdin_config.type = StdIOType::NULL_IO;
  options.stdout_config.type = StdIOType::NULL_IO;
  options.stderr_config.type = StdIOType::NULL_IO;

  ASSERT_TRUE(process.Launch(command_line, options));
  ASSERT_TRUE(listener.launch_succeeded.load(std::memory_order_acquire));
  ASSERT_TRUE(process.Terminate(137, true));
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));

  EXPECT_TRUE(listener.terminated.load(std::memory_order_acquire));
#if defined(_WIN32)
  EXPECT_EQ(listener.exit_state.load(std::memory_order_acquire), ProcessState::kExited);
  EXPECT_EQ(listener.exit_code.load(std::memory_order_acquire), 137);
#else
  EXPECT_EQ(listener.exit_state.load(std::memory_order_acquire), ProcessState::kCrashed);
  EXPECT_EQ(listener.exit_code.load(std::memory_order_acquire), SIGKILL);
#endif
}

TEST(ChildProcessTest, TerminateGracefulStopsRunningProcess) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  CapturingProcessListener listener(&done);
#if !defined(_WIN32)
  WaitableEvent ready(WaitableEvent::ResetPolicy::kAutomatic, false);
#endif
  const scoped_refptr<ProcessService> service = ProcessService::Create();
  ASSERT_TRUE(service);
  ChildProcess process(service);
  process.SetListener(&listener);

#if defined(_WIN32)
  const char *argv[] = {"cmd", "/d", "/c", "ping -n 60 127.0.0.1 > nul"};
#else
  const char *argv[] = {"/bin/sh", "-c", "trap 'exit 42' TERM; echo ready; while :; do :; done"};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  ProcessLaunchOptions options;
  options.stdin_config.type = StdIOType::NULL_IO;
#if defined(_WIN32)
  options.stdout_config.type = StdIOType::NULL_IO;
#else
  options.stdout_config.type = StdIOType::PIPE;
#endif
  options.stderr_config.type = StdIOType::NULL_IO;

  ASSERT_TRUE(process.Launch(command_line, options));
  ASSERT_TRUE(listener.launch_succeeded.load(std::memory_order_acquire));

#if !defined(_WIN32)
  {
    AsyncInputStream *stdout_stream = process.GetStdoutStream();
    ASSERT_NE(stdout_stream, nullptr);
    // Use a shared_ptr<string> so we can accumulate across multiple chunks.
    auto accumulated = std::make_shared<std::string>();
    auto pull = std::make_shared<PullReadState>();
    pull->stream = stdout_stream;
    pull->on_chunk = [accumulated, &ready](const char *data, std::size_t n) {
      accumulated->append(data, n);
      if (NormalizePipeText(*accumulated) == "ready") {
        ready.Signal();
      }
    };
    pull->on_eof = []() {};
    IssuePullRead(pull);
  }
  ASSERT_TRUE(ready.TimedWait(std::chrono::seconds(5)));
#endif

  ASSERT_TRUE(process.Terminate(0, false));
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));

  EXPECT_TRUE(listener.terminated.load(std::memory_order_acquire));
#if defined(_WIN32)
  EXPECT_EQ(listener.exit_state.load(std::memory_order_acquire), ProcessState::kExited);
#else
  EXPECT_EQ(listener.exit_state.load(std::memory_order_acquire), ProcessState::kExited);
  EXPECT_EQ(listener.exit_code.load(std::memory_order_acquire), 42);
#endif
}

TEST(ChildProcessTest, KillOnDestructionWithParentDeathPolicyCanCoexistWithTerminate) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  CapturingProcessListener listener(&done);

  const scoped_refptr<ProcessService> service = ProcessService::Create();
  ASSERT_TRUE(service);
  auto process = std::make_unique<ChildProcess>(service);
  process->SetListener(&listener);

#if defined(_WIN32)
  const char *argv[] = {"cmd", "/d", "/c", "ping -n 60 127.0.0.1 > nul"};
#else
  const char *argv[] = {"/bin/sh", "-c", "while :; do sleep 1; done"};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  ProcessLaunchOptions options;
  options.stdin_config.type = StdIOType::NULL_IO;
  options.stdout_config.type = StdIOType::NULL_IO;
  options.stderr_config.type = StdIOType::NULL_IO;
  options.kill_on_destruction = true;
  options.resource_limits.kill_on_parent_death = true;

  ASSERT_TRUE(process->Launch(command_line, options));
  ASSERT_TRUE(process->Terminate(137, true));
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));
  EXPECT_TRUE(listener.terminated.load(std::memory_order_acquire));
  process.reset();
}

// Regression test: exercises the handle cleanup path when all three
// stdio pipes are PIPE type.  Before the double-close fix this would
// crash in Debug builds because child handles were closed individually
// AND then again by CleanupPipe (child_std == pipe.child_handle).
TEST(ChildProcessTest, LaunchWithAllThreePipesStressHandleCleanup) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  CapturingProcessListener listener(&done);
  const scoped_refptr<ProcessService> service = ProcessService::Create();
  ASSERT_TRUE(service);
  ChildProcess process(service);
  process.SetListener(&listener);

#if defined(_WIN32)
  const char *argv[] = {"cmd", "/d", "/c", "echo ok & exit /b 0"};
#else
  const char *argv[] = {"/bin/sh", "-c", "echo ok; exit 0"};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  // All three stdio handles use PIPE  --  this is the worst-case for the
  // cleanup path: after CreateProcessW every child handle aliases a
  // pipe and must be closed exactly once.
  ProcessLaunchOptions options;
  options.stdin_config.type = StdIOType::PIPE;
  options.stdout_config.type = StdIOType::PIPE;
  options.stderr_config.type = StdIOType::PIPE;

  const bool ok = process.Launch(command_line, options);
  ASSERT_TRUE(ok);
  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));

  EXPECT_TRUE(listener.launch_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(listener.launch_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(listener.terminated.load(std::memory_order_acquire));
  EXPECT_EQ(listener.exit_state.load(std::memory_order_acquire), ProcessState::kExited);
  EXPECT_EQ(listener.exit_code.load(std::memory_order_acquire), 0);
}

#if !defined(_WIN32)
TEST(ChildProcessTest, LinuxResourceLimitsAreApplied) {
  WaitableEvent line_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent terminated_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto state = std::make_shared<LaunchTestState>();
  state->process_service = ProcessService::Create();
  ASSERT_TRUE(state->process_service);
  state->process = std::make_unique<ChildProcess>(state->process_service);
  state->listener = std::make_unique<CapturingProcessListener>(&terminated_done);
  state->process->SetListener(state->listener.get());

  const char *argv[] = {"/bin/sh", "-c", "cat /proc/self/limits"};
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  ProcessLaunchOptions options;
  options.stdout_config.type = StdIOType::PIPE;
  options.stdin_config.type = StdIOType::NULL_IO;
  options.stderr_config.type = StdIOType::NULL_IO;
  options.resource_limits.max_virtual_memory = 64LL * 1024LL * 1024LL;
  options.resource_limits.max_file_descriptors = 64;

  ASSERT_TRUE(state->process->Launch(command_line, options));
  AsyncInputStream *stdout_stream = state->process->GetStdoutStream();
  ASSERT_NE(stdout_stream, nullptr);

  auto pull = std::make_shared<PullReadState>();
  pull->stream = stdout_stream;
  pull->on_chunk = [state, &line_done](const char *data, std::size_t n) {
    state->captured_bytes.insert(state->captured_bytes.end(), data, data + n);
    state->captured_line = std::string(state->captured_bytes.begin(), state->captured_bytes.end());
    line_done.Signal();
  };
  pull->on_eof = []() {};
  IssuePullRead(pull);

  ASSERT_TRUE(line_done.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(terminated_done.TimedWait(std::chrono::seconds(5)));

  const std::vector<std::string> lines = SplitLines(state->captured_line);
  bool saw_as = false;
  bool saw_nofile = false;
  for (const std::string &line : lines) {
    if (line.find("Max address space") != std::string::npos && line.find("67108864") != std::string::npos) {
      saw_as = true;
    }
    if (line.find("Max open files") != std::string::npos && line.find("64") != std::string::npos) {
      saw_nofile = true;
    }
  }

  EXPECT_TRUE(saw_as);
  EXPECT_TRUE(saw_nofile);
  state->process.reset();
  state->process_service.reset();
}
#endif

TEST(ChildProcessTest, LaunchWithWorkingDirectory) {
  WaitableEvent line_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  WaitableEvent terminated_done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto state = std::make_shared<LaunchTestState>();
  state->process_service = ProcessService::Create();
  ASSERT_TRUE(state->process_service);
  state->process = std::make_unique<ChildProcess>(state->process_service);
  state->listener = std::make_unique<CapturingProcessListener>(&terminated_done);

  // Choose a known directory that exists on all platforms.
  // /tmp on POSIX, %TEMP% on Windows — both always exist.
#if defined(_WIN32)
  const char *argv[] = {"cmd", "/d", "/c", "cd"};
  // Use %TEMP% instead of hardcoded C:\Windows.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
  const char *temp = getenv("TEMP");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
  ASSERT_NE(temp, nullptr) << "TEMP environment variable not set";
  const std::string expected_dir = temp;
#else
  const char *argv[] = {"/bin/sh", "-c", "pwd"};
  const std::string expected_dir = "/tmp";
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  ProcessLaunchOptions launch_options;
  launch_options.stdout_config.type = StdIOType::PIPE;
  launch_options.stdin_config.type = StdIOType::NULL_IO;
  launch_options.stderr_config.type = StdIOType::NULL_IO;
  launch_options.working_directory = expected_dir;

  state->process->SetListener(state->listener.get());
  ASSERT_TRUE(state->process->Launch(command_line, launch_options));

  AsyncInputStream *stdout_stream = state->process->GetStdoutStream();
  ASSERT_NE(stdout_stream, nullptr);

  auto pull = std::make_shared<PullReadState>();
  pull->stream = stdout_stream;
  pull->on_chunk = [state, &line_done](const char *data, std::size_t n) {
    state->captured_bytes.insert(state->captured_bytes.end(), data, data + n);
    state->captured_line = std::string(state->captured_bytes.begin(), state->captured_bytes.end());
    line_done.Signal();
  };
  pull->on_eof = []() {};
  IssuePullRead(pull);

  ASSERT_TRUE(line_done.TimedWait(std::chrono::seconds(5)));
  ASSERT_TRUE(terminated_done.TimedWait(std::chrono::seconds(5)));

  const std::string normalized = NormalizePipeText(state->captured_line);
  EXPECT_EQ(normalized, expected_dir);

  state->process.reset();
  state->process_service.reset();
}

} // namespace
} // namespace nei