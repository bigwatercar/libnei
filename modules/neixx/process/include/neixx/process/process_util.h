#pragma once

#ifndef NEIXX_PROCESS_PROCESS_UTIL_H_
#define NEIXX_PROCESS_PROCESS_UTIL_H_

#include <limits>

#include <nei/macros/nei_export.h>
#include <neixx/common/time.h>
#include <neixx/process/child_process.h>

namespace nei {

class CommandLine;

struct ElevatedProcessOptions {
  bool inherit_console = false;
  TimeDelta wait_timeout =
      TimeDelta::FromMicroseconds(std::numeric_limits<long long>::max());
};

class NEI_API ProcessUtil {
 public:
  static ProcessExitInfo LaunchProcessElevated(
      const CommandLine& command_line,
      const ElevatedProcessOptions& options);
};

}  // namespace nei

#endif  // NEIXX_PROCESS_PROCESS_UTIL_H_
