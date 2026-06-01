#pragma once

#ifndef NEIXX_PROCESS_CHILD_PROCESS_IMPL_INTERFACE_H_
#define NEIXX_PROCESS_CHILD_PROCESS_IMPL_INTERFACE_H_

#include <neixx/process/child_process.h>

namespace nei {

class ChildProcess::Impl {
 public:
  virtual ~Impl() = default;

  virtual bool Launch(const CommandLine& command_line,
                      const ProcessLaunchOptions& options,
                      ChildProcessListener* listener) = 0;
  virtual bool Terminate(int exit_code, bool force) = 0;
  virtual void SetExternalListener(ChildProcessListener* listener) = 0;
  virtual AsyncInputStream* GetStdoutStream() const = 0;
  virtual AsyncInputStream* GetStderrStream() const = 0;
  virtual AsyncOutputStream* GetStdinStream() const = 0;
};

}  // namespace nei

#endif  // NEIXX_PROCESS_CHILD_PROCESS_IMPL_INTERFACE_H_
