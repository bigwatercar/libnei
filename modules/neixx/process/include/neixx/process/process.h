#ifndef NEIXX_PROCESS_PROCESS_H_
#define NEIXX_PROCESS_PROCESS_H_

#include <cstdint>
#include <memory>

#include <nei/macros/nei_export.h>
#include <neixx/io/async_handle.h>
#include <neixx/io/file_handle.h>
#include <neixx/task/task_runner.h>

namespace nei {

class CommandLine;
struct LaunchOptions;

class NEI_API Process final {
public:
  class Impl;

  Process();
  ~Process();

  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;

  Process(Process &&) noexcept;
  Process &operator=(Process &&) noexcept;

  bool is_valid() const;
  std::uint64_t id() const;

  int Wait();
  bool Terminate(int exit_code = 1);
  bool IsRunning() const;

  void SetTerminationCallback(std::shared_ptr<TaskRunner> task_runner, OnceClosure callback);

  std::unique_ptr<AsyncHandle> TakeStdinStream();
  std::unique_ptr<AsyncHandle> TakeStdoutStream();
  std::unique_ptr<AsyncHandle> TakeStderrStream();

private:
  explicit Process(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend Process LaunchProcess(const CommandLine &command_line, LaunchOptions options);
};

} // namespace nei

#endif // NEIXX_PROCESS_PROCESS_H_
