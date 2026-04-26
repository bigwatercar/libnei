#include <iostream>
#include <string>
#include <vector>

#include <neixx/command_line/command_line.h>

int main(int argc, char **argv) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  nei::CommandLine::Init();
#else
  nei::CommandLine::Init(argc, argv);
#endif

  nei::CommandLine &command_line = nei::CommandLine::ForCurrentProcess();

  std::cout << "Has --test: " << (command_line.HasSwitch("test") ? "true" : "false") << "\n";
  std::cout << "Command line: " << command_line.GetCommandLineString() << "\n";

  const std::vector<std::string> args = command_line.GetArgs();
  std::cout << "Positional args (" << args.size() << "):" << "\n";
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::cout << "  [" << i << "] " << args[i] << "\n";
  }

  return 0;
}
