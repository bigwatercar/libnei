#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <neixx/command_line/command_line.h>
#include <neixx/io/async_line_reader.h>
#include <neixx/process/child_process.h>
#include <neixx/synchronization/waitable_event.h>

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

  void OnProcessTerminated(const ProcessExitInfo& /*info*/) override {
    terminated.fetch_add(1, std::memory_order_relaxed);
  }

  std::atomic<int> launch_succeeded{0};
  std::atomic<int> launch_failed{0};
  std::atomic<int> terminated{0};
};

class CapturingProcessListener final : public ChildProcessListener {
 public:
  explicit CapturingProcessListener(WaitableEvent* done_event)
      : done_event_(done_event) {}

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

  void OnProcessTerminated(const ProcessExitInfo& info) override {
    exit_state.store(info.state, std::memory_order_release);
    exit_code.store(info.exit_code, std::memory_order_release);
    terminated.store(true, std::memory_order_release);
    if (done_event_ != nullptr) {
      done_event_->Signal();
    }
  }

  WaitableEvent* done_event_ = nullptr;
  std::atomic<bool> launch_succeeded{false};
  std::atomic<bool> launch_failed{false};
  std::atomic<bool> terminated{false};
  std::atomic<int> launched_pid{-1};
  std::atomic<ProcessState> exit_state{ProcessState::kNotStarted};
  std::atomic<int> exit_code{-1};
};

struct LaunchTestState {
  std::unique_ptr<ChildProcess> process;
  std::unique_ptr<AsyncLineReader> line_reader;
  std::unique_ptr<CapturingProcessListener> listener;
  std::atomic<bool> post_completed{false};
  std::atomic<bool> saw_line{false};
  std::atomic<bool> launch_returned{false};
  std::atomic<bool> write_callback_called{false};
  std::atomic<bool> write_success{false};
  std::string captured_line;
};

TEST(ChildProcessTest, LaunchWorksWithoutCurrentIoPump) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  CapturingProcessListener listener(&done);
  ChildProcess process;
  process.SetListener(&listener);

#if defined(_WIN32)
  const char* argv[] = {"cmd", "/d", "/c", "exit /b 0"};
#else
  const char* argv[] = {"/bin/sh", "-c", "exit 0"};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])),
                           argv);

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
  EXPECT_EQ(listener.exit_state.load(std::memory_order_acquire),
            ProcessState::kExited);
  EXPECT_EQ(listener.exit_code.load(std::memory_order_acquire), 0);
}

TEST(ChildProcessTest, LaunchWithStdoutPipeReadsLine) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto state = std::make_shared<LaunchTestState>();
  state->process = std::make_unique<ChildProcess>();
  state->listener = std::make_unique<CapturingProcessListener>(&done);

#if defined(_WIN32)
  const char* argv[] = {"cmd", "/d", "/c", "echo child-out"};
#else
  const char* argv[] = {"/bin/sh", "-c", "printf 'child-out\\n'"};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])),
                           argv);

  ProcessLaunchOptions launch_options;
  launch_options.stdout_config.type = StdIOType::PIPE;
  state->process->SetListener(state->listener.get());
  const bool ok = state->process->Launch(command_line, launch_options);
  state->launch_returned.store(ok, std::memory_order_release);

  if (!ok) {
    done.Signal();
  } else {
    AsyncInputStream* stdout_stream = state->process->GetStdoutStream();
    if (stdout_stream == nullptr) {
      done.Signal();
    } else {
      state->line_reader = std::make_unique<AsyncLineReader>(stdout_stream);
      state->line_reader->StartReadingLines([state, &done](std::string&& line) {
        state->captured_line = std::move(line);
        state->saw_line.store(true, std::memory_order_release);
        done.Signal();
      });

      state->post_completed.store(true, std::memory_order_release);
    }
  }

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));

  EXPECT_TRUE(state->post_completed.load(std::memory_order_acquire) ||
              state->launch_returned.load(std::memory_order_acquire) == false);
  EXPECT_TRUE(state->launch_returned.load(std::memory_order_acquire));
  EXPECT_TRUE(state->listener->launch_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(state->listener->launch_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(state->saw_line.load(std::memory_order_acquire));
  EXPECT_EQ(state->captured_line, "child-out");
  if (state->listener->terminated.load(std::memory_order_acquire)) {
    EXPECT_EQ(state->listener->exit_state.load(std::memory_order_acquire),
              ProcessState::kExited);
    EXPECT_EQ(state->listener->exit_code.load(std::memory_order_acquire), 0);
  }
}

TEST(ChildProcessTest, LaunchWithStdinPipeEchoesToStdout) {
  WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
  auto state = std::make_shared<LaunchTestState>();
  state->process = std::make_unique<ChildProcess>();
  state->listener = std::make_unique<CapturingProcessListener>(&done);

#if defined(_WIN32)
  const char* argv[] = {"cmd", "/d", "/v:on", "/c",
              "set /p line=& echo !line!"};
#else
  const char* argv[] = {"/bin/sh", "-c",
              "IFS= read -r line; printf '%s\\n' \"$line\""};
#endif
  CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])),
                           argv);

  ProcessLaunchOptions launch_options;
  launch_options.stdin_config.type = StdIOType::PIPE;
  launch_options.stdout_config.type = StdIOType::PIPE;
  state->process->SetListener(state->listener.get());
  const bool ok = state->process->Launch(command_line, launch_options);
  state->launch_returned.store(ok, std::memory_order_release);
  if (!ok) {
    done.Signal();
  } else {
    AsyncInputStream* stdout_stream = state->process->GetStdoutStream();
    AsyncOutputStream* stdin_stream = state->process->GetStdinStream();
    if (stdout_stream == nullptr || stdin_stream == nullptr) {
      done.Signal();
    } else {
      state->line_reader = std::make_unique<AsyncLineReader>(stdout_stream);
      state->line_reader->StartReadingLines([state, &done](std::string&& line) {
        state->captured_line = std::move(line);
        state->saw_line.store(true, std::memory_order_release);
        done.Signal();
      });

      const std::string payload = "echo-through-stdin\n";
      std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
      stdin_stream->WriteAsync(std::move(bytes), [state](bool success) {
        state->write_callback_called.store(true, std::memory_order_release);
        state->write_success.store(success, std::memory_order_release);
      });

      state->post_completed.store(true, std::memory_order_release);
    }
  }

  ASSERT_TRUE(done.TimedWait(std::chrono::seconds(5)));

  EXPECT_TRUE(state->post_completed.load(std::memory_order_acquire) ||
              state->launch_returned.load(std::memory_order_acquire) == false);
  EXPECT_TRUE(state->launch_returned.load(std::memory_order_acquire));
  EXPECT_TRUE(state->listener->launch_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(state->listener->launch_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(state->write_callback_called.load(std::memory_order_acquire));
  EXPECT_TRUE(state->write_success.load(std::memory_order_acquire));
  EXPECT_TRUE(state->saw_line.load(std::memory_order_acquire));
  EXPECT_EQ(state->captured_line, "echo-through-stdin");
}

}  // namespace
}  // namespace nei
