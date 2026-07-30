#include <iostream>

#include <neixx/command_line/command_line.h>
#include <neixx/process/process_util.h>

int main() {
#if defined(_WIN32)
  // Construct command line to launch calc.exe with no extra arguments.
  const char *argv[] = {R"(C:\Windows\System32\calc.exe)"};
  nei::CommandLine command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv);

  nei::ElevatedProcessOptions options;
  options.inherit_console = false;

  std::cout << "Launching elevated: " << command_line.GetProgram() << "\n";
  const nei::ProcessExitInfo info = nei::ProcessUtil::LaunchProcessElevated(command_line, options);

  switch (info.state) {
  case nei::ProcessState::kExited:
    std::cout << "Process exited with code: " << info.exit_code << "\n";
    break;
  case nei::ProcessState::kRunning:
    std::cout << "Process is running (non-blocking / timeout).\n";
    break;
  case nei::ProcessState::kFailedToStart:
    std::cout << "Failed to start process.\n";
    break;
  case nei::ProcessState::kCrashed:
    std::cout << "Process crashed.\n";
    break;
  default:
    std::cout << "Unknown process state.\n";
    break;
  }
#else
  std::cout << "This demo is Windows-only.\n";
#endif
  return 0;
}
