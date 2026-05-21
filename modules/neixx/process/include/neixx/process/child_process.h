#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_H_
#define NEIXX_PROCESS_CHILD_PROCESS_H_

#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_stream.h>
#include <neixx/task/message_loop/message_pump_io.h>

namespace nei {

class CommandLine;

enum class StdIOType {
  INHERIT,
  NULL_IO,
  PIPE,
  REDIRECT,
};

struct StdIOConfig {
  StdIOType type = StdIOType::INHERIT;
  NativeIOHandle target_handle = NativeIOHandle{};
};

struct ProcessLaunchOptions {
  StdIOConfig stdin_config;
  StdIOConfig stdout_config;
  StdIOConfig stderr_config;
};

enum class ProcessState {
  kNotStarted,
  kRunning,
  kExited,
  kCrashed,
  kFailedToStart,
};

struct ProcessExitInfo {
  ProcessState state = ProcessState::kNotStarted;
  int exit_code = -1;
};

class NEI_API ChildProcessListener {
 public:
  virtual ~ChildProcessListener() = default;

  virtual void OnProcessLaunchSucceeded(int pid) = 0;
  virtual void OnProcessLaunchFailed() = 0;
  virtual void OnProcessTerminated(const ProcessExitInfo& info) = 0;
};

class NEI_API ChildProcess : public MessagePumpForIO::Watcher {
 public:
  ChildProcess();
  ~ChildProcess() override;

  bool Launch(const CommandLine& command_line,
              const ProcessLaunchOptions& options);

  void SetListener(ChildProcessListener* listener);

  AsyncInputStream* GetStdoutStream() const;
  AsyncInputStream* GetStderrStream() const;
  AsyncOutputStream* GetStdinStream() const;

  void OnFileCanReadWithoutBlocking(NativeIOHandle handle) override;
  void OnFileCanWriteWithoutBlocking(NativeIOHandle handle) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  ChildProcessListener* listener_ = nullptr;
};

}  // namespace nei

#endif  // NEIXX_PROCESS_CHILD_PROCESS_H_
