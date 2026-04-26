#ifndef NEIXX_COMMAND_LINE_COMMAND_LINE_H_
#define NEIXX_COMMAND_LINE_COMMAND_LINE_H_

#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nei/macros/nei_export.h>

namespace nei {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

class NEI_API CommandLine final {
public:
  class Impl;

  // UTF-16 token sequence used by wrapper/raw/full argv views.
  using StringVector = std::vector<std::u16string>;
  // Normalized switch map: switch name (ASCII-folded, no prefix) -> value.
  using SwitchMap = std::map<std::u16string, std::u16string>;

  // Behavior when appending/copying a switch that already exists.
  enum class DuplicateSwitchPolicy {
    // Replace existing value and update argv token representation.
    kReplaceExisting,
    // Keep existing value and ignore incoming duplicate.
    kPreserveExisting,
  };

  // Value-level filter when copying switches from another CommandLine.
  enum class SwitchValueFilter {
    // Copy all switches.
    kAll,
    // Copy only switches with non-empty values (for example --name=value).
    kWithValueOnly,
    // Copy only switches without values (boolean flags, for example --flag).
    kWithoutValueOnly,
  };

  // Fine-grained options for CopySwitchesFrom.
  struct CopySwitchesOptions {
    // Optional allow-list. Empty means no allow-list filtering.
    std::vector<std::string> whitelist;
    // Optional deny-list. Applied after whitelist filtering.
    std::vector<std::string> blacklist;
    // Optional value-based filter.
    SwitchValueFilter value_filter = SwitchValueFilter::kAll;
    // Duplicate handling policy for copied switches.
    DuplicateSwitchPolicy policy = DuplicateSwitchPolicy::kReplaceExisting;
  };

  // Constructs a command line object. On Windows this can parse current process
  // command line; on non-Windows this is usually an empty command line unless
  // populated via append/copy APIs.
  CommandLine();
  // Parses command line from argv.
  explicit CommandLine(int argc, const char *const *argv);
  ~CommandLine();

  CommandLine(const CommandLine &) = delete;
  CommandLine &operator=(const CommandLine &) = delete;

  CommandLine(CommandLine &&) noexcept;
  CommandLine &operator=(CommandLine &&) noexcept;

  // Initializes process-global CommandLine from argc/argv.
  static void Init(int argc, const char *const *argv);
#if defined(_WIN32)
  // Initializes process-global CommandLine from GetCommandLineW().
  static void Init();
#endif
  // Returns process-global CommandLine. Lazily initializes if needed.
  static CommandLine &ForCurrentProcess();

  // Returns child program token (raw argv[0]) in UTF-8/UTF-16.
  std::string GetProgram() const;
  std::u16string GetProgramUTF16() const;

  // Switch presence queries. Names may include prefix, matching is
  // ASCII case-insensitive after normalization.
  bool HasSwitch(std::string_view name) const;
  bool HasSwitch(std::u16string_view name) const;

  // Switch value queries. Return empty string if switch is missing or has no
  // explicit value.
  std::string GetSwitchValueASCII(std::string_view name) const;
  std::string GetSwitchValueASCII(std::u16string_view name) const;
  std::u16string GetSwitchValueUTF16(std::string_view name) const;
  std::u16string GetSwitchValueUTF16(std::u16string_view name) const;

  // Returns normalized switch map (read-only view).
  const SwitchMap &GetSwitches() const;

  // Appends a boolean switch (no explicit value).
  void AppendSwitch(std::string_view name,
                    DuplicateSwitchPolicy policy = DuplicateSwitchPolicy::kReplaceExisting);
  // Appends a UTF-8 switch/value pair.
  void AppendSwitchASCII(std::string_view name,
                         std::string_view value,
                         DuplicateSwitchPolicy policy = DuplicateSwitchPolicy::kReplaceExisting);
  // Appends a UTF-16 switch/value pair.
  void AppendSwitchUTF16(std::u16string_view name,
                         std::u16string_view value,
                         DuplicateSwitchPolicy policy = DuplicateSwitchPolicy::kReplaceExisting);

  // Copies switches from source using a simple allow-list + duplicate policy.
  void CopySwitchesFrom(const CommandLine &source,
                        const std::vector<std::string> &whitelist = {},
                        DuplicateSwitchPolicy policy = DuplicateSwitchPolicy::kReplaceExisting);
  // Copies switches from source using full options.
  void CopySwitchesFrom(const CommandLine &source, const CopySwitchesOptions &options);

  // Prepends wrapper command line tokens before raw child argv.
  void PrependWrapper(const CommandLine &wrapper);
  // Prepends wrapper program only (UTF-8).
  void PrependWrapper(std::string_view wrapper_program);
  // Prepends wrapper program + args (UTF-8).
  void PrependWrapper(std::string_view wrapper_program, const std::vector<std::string> &wrapper_args);
  // Prepends wrapper program only (UTF-16).
  void PrependWrapperUTF16(std::u16string_view wrapper_program);
  // Prepends wrapper program + args (UTF-16).
  void PrependWrapperUTF16(std::u16string_view wrapper_program, const StringVector &wrapper_args);

  // Positional arguments (non-switch tokens).
  std::vector<std::string> GetArgs() const;
  const StringVector &GetArgsUTF16() const;

  // argv views:
  // - wrapper: wrapper-only tokens
  // - raw: child-only tokens
  // - full: wrapper + raw
  const StringVector &GetWrapperArgv() const;
  const StringVector &GetRawArgv() const;
  const StringVector &GetFullArgv() const;
  // Backward-compatible alias of GetFullArgv().
  const StringVector &argv() const;

  // Appends positional argument (UTF-8).
  void AppendArg(std::string_view value);
  // Appends another command line's argv tokens, optionally including source
  // program token, then re-parses current raw command line.
  void AppendArguments(const CommandLine &source, bool include_program);

  // Returns printable full command line string with quoting/escaping applied.
  std::string GetCommandLineString() const;

private:
  void ParseFromArgv(int argc, const char *const *argv);
  void ParseFromUTF16Argv(const std::vector<std::u16string> &argv);

  std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace nei

#endif // NEIXX_COMMAND_LINE_COMMAND_LINE_H_
