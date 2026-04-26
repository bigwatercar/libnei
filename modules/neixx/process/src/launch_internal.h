#ifndef NEIXX_PROCESS_LAUNCH_INTERNAL_H_
#define NEIXX_PROCESS_LAUNCH_INTERNAL_H_

#include <memory>

#include <neixx/process/launch.h>

namespace nei {

namespace detail {

std::unique_ptr<Process::Impl> LaunchProcessImpl(const CommandLine &command_line, LaunchOptions options);

} // namespace detail

} // namespace nei

#endif // NEIXX_PROCESS_LAUNCH_INTERNAL_H_
