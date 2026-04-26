#include <neixx/process/launch.h>

#include <utility>

#include "launch_internal.h"
#include "process_internal.h"

namespace nei {

Process LaunchProcess(const CommandLine &command_line, LaunchOptions options) {
  return Process(detail::LaunchProcessImpl(command_line, std::move(options)));
}

} // namespace nei
