#include <neixx/command_line/command_line.h>

#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <neixx/strings/string_util.h>
#include <neixx/strings/utf_string_conversions.h>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#endif

namespace nei {

namespace {

std::u16string::size_type FindSwitchValueSeparator(std::u16string_view token) {
  const std::u16string::size_type equals_pos = token.find(u'=');

#if defined(_WIN32)
  const bool slash_prefixed = !token.empty() && token[0] == u'/';
  const std::u16string::size_type colon_pos = slash_prefixed ? token.find(u':') : std::u16string::npos;

  if (equals_pos == std::u16string::npos) {
    return colon_pos;
  }
  if (colon_pos == std::u16string::npos) {
    return equals_pos;
  }
  return equals_pos < colon_pos ? equals_pos : colon_pos;
#else
  return equals_pos;
#endif
}

bool IsSwitchToken(std::u16string_view token) {
  if (token.size() >= 3 && token[0] == u'-' && token[1] == u'-') {
    return true;
  }
  if (token.size() >= 2 && token[0] == u'-') {
    return true;
  }
#if defined(_WIN32)
  if (token.size() >= 2 && token[0] == u'/') {
    return true;
  }
#endif
  return false;
}

std::u16string RemoveSwitchPrefix(std::u16string_view token) {
  if (token.size() >= 2 && token[0] == u'-' && token[1] == u'-') {
    return std::u16string(token.substr(2));
  }
  if (!token.empty() && (token[0] == u'-' || token[0] == u'/')) {
    return std::u16string(token.substr(1));
  }
  return std::u16string(token);
}

std::u16string NormalizeSwitchName(std::u16string_view name) {
  return ToLowerASCII(RemoveSwitchPrefix(name));
}

bool ParseSwitchToken(std::u16string_view token,
                      std::u16string *name,
                      std::u16string *value) {
  if (!IsSwitchToken(token) || token == u"--") {
    return false;
  }

  const std::u16string without_prefix = RemoveSwitchPrefix(token);
  if (without_prefix.empty()) {
    return false;
  }

  const std::u16string::size_type separator_pos = FindSwitchValueSeparator(token);
  if (separator_pos == std::u16string::npos) {
    *name = NormalizeSwitchName(without_prefix);
    value->clear();
    return !name->empty();
  }

  const std::size_t prefix_length = token.size() - without_prefix.size();
  const std::u16string::size_type value_offset = separator_pos + 1;

  *name = NormalizeSwitchName(token.substr(prefix_length, separator_pos - prefix_length));
  *value = std::u16string(token.substr(value_offset));
  return !name->empty();
}

std::size_t FindSwitchInsertionIndex(const std::vector<std::u16string> &argv) {
  if (argv.empty()) {
    return 0;
  }

  for (std::size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == u"--") {
      return i;
    }

    std::u16string switch_name;
    std::u16string switch_value;
    if (!ParseSwitchToken(argv[i], &switch_name, &switch_value)) {
      return i;
    }
  }

  return argv.size();
}

std::u16string QuoteForCommandLine(std::u16string_view arg) {
  if (arg.empty()) {
    return u"\"\"";
  }

  bool needs_quotes = false;
  for (const char16_t ch : arg) {
    if (ch == u' ' || ch == u'\t' || ch == u'\"') {
      needs_quotes = true;
      break;
    }
  }

  if (!needs_quotes) {
    return std::u16string(arg);
  }

  std::u16string out;
  out.push_back(u'\"');

  std::size_t backslash_count = 0;
  for (const char16_t ch : arg) {
    if (ch == u'\\') {
      ++backslash_count;
      continue;
    }

    if (ch == u'\"') {
      out.append(backslash_count * 2 + 1, u'\\');
      out.push_back(u'\"');
      backslash_count = 0;
      continue;
    }

    if (backslash_count > 0) {
      out.append(backslash_count, u'\\');
      backslash_count = 0;
    }
    out.push_back(ch);
  }

  if (backslash_count > 0) {
    out.append(backslash_count * 2, u'\\');
  }
  out.push_back(u'\"');

  return out;
}

std::vector<std::u16string> ParseWindowsCommandLine() {
  std::vector<std::u16string> argv;
#if defined(_WIN32)
  int argc = 0;
  LPWSTR *wide_argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (wide_argv == nullptr || argc <= 0) {
    if (wide_argv != nullptr) {
      ::LocalFree(wide_argv);
    }
    return argv;
  }

  argv.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    const wchar_t *wide = wide_argv[i] != nullptr ? wide_argv[i] : L"";
    argv.emplace_back(reinterpret_cast<const char16_t *>(wide));
  }

  ::LocalFree(wide_argv);
#endif
  return argv;
}

std::mutex &CurrentProcessMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unique_ptr<CommandLine> &CurrentProcessCommandLine() {
  static std::unique_ptr<CommandLine> current;
  return current;
}

void RemoveSwitchTokens(std::vector<std::u16string> *argv, std::u16string_view name) {
  for (std::size_t i = 1; i < argv->size();) {
    if ((*argv)[i] == u"--") {
      break;
    }

    std::u16string switch_name;
    std::u16string switch_value;
    if (ParseSwitchToken((*argv)[i], &switch_name, &switch_value) && switch_name == name) {
      argv->erase(argv->begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    ++i;
  }
}

void MarkFullArgvDirty(CommandLine::Impl *impl);

} // namespace

class CommandLine::Impl {
public:
  std::vector<std::u16string> wrapper_argv;
  std::vector<std::u16string> argv;
  std::map<std::u16string, std::u16string> switches;
  std::vector<std::u16string> args;
  mutable std::vector<std::u16string> full_argv_cache;
  mutable bool full_argv_dirty = true;
};

namespace {

void MarkFullArgvDirty(CommandLine::Impl *impl) {
  impl->full_argv_dirty = true;
}

const CommandLine::StringVector &GetCachedFullArgv(const CommandLine::Impl *impl) {
  if (!impl->full_argv_dirty) {
    return impl->full_argv_cache;
  }

  impl->full_argv_cache.clear();
  impl->full_argv_cache.reserve(impl->wrapper_argv.size() + impl->argv.size());
  impl->full_argv_cache.insert(impl->full_argv_cache.end(), impl->wrapper_argv.begin(), impl->wrapper_argv.end());
  impl->full_argv_cache.insert(impl->full_argv_cache.end(), impl->argv.begin(), impl->argv.end());
  impl->full_argv_dirty = false;
  return impl->full_argv_cache;
}

void AppendSwitchToken(std::vector<std::u16string> *argv,
                       std::map<std::u16string, std::u16string> *switches,
                       std::u16string_view name,
                       std::u16string_view value,
                       CommandLine::DuplicateSwitchPolicy policy) {
  const std::u16string normalized_name = NormalizeSwitchName(name);
  if (normalized_name.empty()) {
    return;
  }

  if (policy == CommandLine::DuplicateSwitchPolicy::kPreserveExisting &&
      switches->find(normalized_name) != switches->end()) {
    return;
  }

  const std::u16string value16(value);
  (*switches)[normalized_name] = value16;
  RemoveSwitchTokens(argv, normalized_name);

  std::u16string token = u"--";
  token += normalized_name;
  if (!value16.empty()) {
    token += u"=";
    token += value16;
  }
  argv->insert(argv->begin() + static_cast<std::ptrdiff_t>(FindSwitchInsertionIndex(*argv)), std::move(token));
}

std::set<std::u16string> BuildWhitelist(const std::vector<std::string> &whitelist) {
  std::set<std::u16string> normalized;
  for (const std::string &name : whitelist) {
    const std::u16string normalized_name = NormalizeSwitchName(UTF8ToUTF16(name));
    if (!normalized_name.empty()) {
      normalized.insert(normalized_name);
    }
  }
  return normalized;
}

void PrependWrapperTokens(std::vector<std::u16string> *wrapper_argv,
                          std::vector<std::u16string> tokens) {
  if (tokens.empty()) {
    return;
  }

  wrapper_argv->insert(wrapper_argv->begin(), tokens.begin(), tokens.end());
}

} // namespace

CommandLine::CommandLine()
    : impl_(std::make_unique<Impl>()) {
#if defined(_WIN32)
  ParseFromUTF16Argv(ParseWindowsCommandLine());
#endif
}

CommandLine::CommandLine(int argc, const char *const *argv)
    : impl_(std::make_unique<Impl>()) {
  ParseFromArgv(argc, argv);
}

CommandLine::~CommandLine() = default;

CommandLine::CommandLine(CommandLine &&) noexcept = default;

CommandLine &CommandLine::operator=(CommandLine &&) noexcept = default;

void CommandLine::Init(int argc, const char *const *argv) {
  std::lock_guard<std::mutex> lock(CurrentProcessMutex());
  CurrentProcessCommandLine() = std::make_unique<CommandLine>(argc, argv);
}

#if defined(_WIN32)
void CommandLine::Init() {
  std::lock_guard<std::mutex> lock(CurrentProcessMutex());
  CurrentProcessCommandLine() = std::make_unique<CommandLine>();
}
#endif

CommandLine &CommandLine::ForCurrentProcess() {
  std::lock_guard<std::mutex> lock(CurrentProcessMutex());
  if (!CurrentProcessCommandLine()) {
#if defined(_WIN32)
    CurrentProcessCommandLine() = std::make_unique<CommandLine>();
#else
    CurrentProcessCommandLine() = std::make_unique<CommandLine>();
#endif
  }
  return *CurrentProcessCommandLine();
}

std::string CommandLine::GetProgram() const {
  return UTF16ToUTF8(GetProgramUTF16());
}

std::u16string CommandLine::GetProgramUTF16() const {
  if (impl_->argv.empty()) {
    return std::u16string();
  }
  return impl_->argv.front();
}

bool CommandLine::HasSwitch(std::string_view name) const {
  return HasSwitch(UTF8ToUTF16(name));
}

bool CommandLine::HasSwitch(std::u16string_view name) const {
  const std::u16string key = NormalizeSwitchName(name);
  return impl_->switches.find(key) != impl_->switches.end();
}

std::string CommandLine::GetSwitchValueASCII(std::string_view name) const {
  return GetSwitchValueASCII(UTF8ToUTF16(name));
}

std::string CommandLine::GetSwitchValueASCII(std::u16string_view name) const {
  const std::u16string key = NormalizeSwitchName(name);
  const auto it = impl_->switches.find(key);
  if (it == impl_->switches.end()) {
    return std::string();
  }
  return UTF16ToUTF8(it->second);
}

std::u16string CommandLine::GetSwitchValueUTF16(std::string_view name) const {
  return GetSwitchValueUTF16(UTF8ToUTF16(name));
}

std::u16string CommandLine::GetSwitchValueUTF16(std::u16string_view name) const {
  const std::u16string key = NormalizeSwitchName(name);
  const auto it = impl_->switches.find(key);
  if (it == impl_->switches.end()) {
    return std::u16string();
  }
  return it->second;
}

const CommandLine::SwitchMap &CommandLine::GetSwitches() const {
  return impl_->switches;
}

void CommandLine::AppendSwitch(std::string_view name, DuplicateSwitchPolicy policy) {
  AppendSwitchASCII(name, std::string_view(), policy);
}

void CommandLine::AppendSwitchASCII(std::string_view name,
                                    std::string_view value,
                                    DuplicateSwitchPolicy policy) {
  AppendSwitchUTF16(UTF8ToUTF16(name), UTF8ToUTF16(value), policy);
}

void CommandLine::AppendSwitchUTF16(std::u16string_view name,
                                    std::u16string_view value,
                                    DuplicateSwitchPolicy policy) {
  AppendSwitchToken(&impl_->argv, &impl_->switches, name, value, policy);
  MarkFullArgvDirty(impl_.get());
}

void CommandLine::CopySwitchesFrom(const CommandLine &source,
                                   const std::vector<std::string> &whitelist,
                                   DuplicateSwitchPolicy policy) {
  CopySwitchesOptions options;
  options.whitelist = whitelist;
  options.policy = policy;
  CopySwitchesFrom(source, options);
}

void CommandLine::CopySwitchesFrom(const CommandLine &source, const CopySwitchesOptions &options) {
  const std::set<std::u16string> normalized_whitelist = BuildWhitelist(options.whitelist);
  const std::set<std::u16string> normalized_blacklist = BuildWhitelist(options.blacklist);
  for (const auto &entry : source.GetSwitches()) {
    if (!normalized_whitelist.empty() && normalized_whitelist.find(entry.first) == normalized_whitelist.end()) {
      continue;
    }
    if (normalized_blacklist.find(entry.first) != normalized_blacklist.end()) {
      continue;
    }
    switch (options.value_filter) {
    case SwitchValueFilter::kWithValueOnly:
      if (entry.second.empty()) {
        continue;
      }
      break;
    case SwitchValueFilter::kWithoutValueOnly:
      if (!entry.second.empty()) {
        continue;
      }
      break;
    case SwitchValueFilter::kAll:
    default:
      break;
    }
    AppendSwitchToken(&impl_->argv, &impl_->switches, entry.first, entry.second, options.policy);
  }
  MarkFullArgvDirty(impl_.get());
}

void CommandLine::PrependWrapper(const CommandLine &wrapper) {
  const StringVector &wrapper_tokens = wrapper.argv();
  if (wrapper_tokens.empty()) {
    return;
  }

  PrependWrapperTokens(&impl_->wrapper_argv, wrapper_tokens);
  MarkFullArgvDirty(impl_.get());
}

void CommandLine::PrependWrapper(std::string_view wrapper_program) {
  PrependWrapper(wrapper_program, std::vector<std::string>());
}

void CommandLine::PrependWrapper(std::string_view wrapper_program, const std::vector<std::string> &wrapper_args) {
  StringVector tokens;
  tokens.reserve(wrapper_args.size() + 1);
  tokens.push_back(UTF8ToUTF16(wrapper_program));
  for (const std::string &arg : wrapper_args) {
    tokens.push_back(UTF8ToUTF16(arg));
  }
  PrependWrapperTokens(&impl_->wrapper_argv, std::move(tokens));
  MarkFullArgvDirty(impl_.get());
}

void CommandLine::PrependWrapperUTF16(std::u16string_view wrapper_program) {
  PrependWrapperUTF16(wrapper_program, StringVector());
}

void CommandLine::PrependWrapperUTF16(std::u16string_view wrapper_program, const StringVector &wrapper_args) {
  StringVector tokens;
  tokens.reserve(wrapper_args.size() + 1);
  tokens.push_back(std::u16string(wrapper_program));
  tokens.insert(tokens.end(), wrapper_args.begin(), wrapper_args.end());
  PrependWrapperTokens(&impl_->wrapper_argv, std::move(tokens));
  MarkFullArgvDirty(impl_.get());
}

std::vector<std::string> CommandLine::GetArgs() const {
  std::vector<std::string> out;
  out.reserve(impl_->args.size());
  for (const std::u16string &arg : impl_->args) {
    out.push_back(UTF16ToUTF8(arg));
  }
  return out;
}

const CommandLine::StringVector &CommandLine::GetArgsUTF16() const {
  return impl_->args;
}

const CommandLine::StringVector &CommandLine::GetWrapperArgv() const {
  return impl_->wrapper_argv;
}

const CommandLine::StringVector &CommandLine::GetRawArgv() const {
  return impl_->argv;
}

const CommandLine::StringVector &CommandLine::GetFullArgv() const {
  return GetCachedFullArgv(impl_.get());
}

const CommandLine::StringVector &CommandLine::argv() const {
  return GetFullArgv();
}

void CommandLine::AppendArg(std::string_view value) {
  const std::u16string value16 = UTF8ToUTF16(value);
  impl_->args.push_back(value16);
  impl_->argv.push_back(value16);
  MarkFullArgvDirty(impl_.get());
}

void CommandLine::AppendArguments(const CommandLine &source, bool include_program) {
  std::vector<std::u16string> combined_argv = impl_->argv;
  const StringVector &source_argv = source.argv();

  std::size_t start_index = 0;
  if (!include_program && !source_argv.empty()) {
    start_index = 1;
  }

  combined_argv.insert(combined_argv.end(), source_argv.begin() + static_cast<std::ptrdiff_t>(start_index), source_argv.end());
  ParseFromUTF16Argv(combined_argv);
}

std::string CommandLine::GetCommandLineString() const {
  const StringVector &full_argv = argv();
  if (full_argv.empty()) {
    return std::string();
  }

  std::u16string joined;
  for (std::size_t i = 0; i < full_argv.size(); ++i) {
    if (i != 0) {
      joined.push_back(u' ');
    }
    joined += QuoteForCommandLine(full_argv[i]);
  }
  return UTF16ToUTF8(joined);
}

void CommandLine::ParseFromArgv(int argc, const char *const *argv) {
  std::vector<std::u16string> utf16_argv;
  if (argc > 0) {
    utf16_argv.reserve(static_cast<std::size_t>(argc));
  }

  for (int i = 0; i < argc; ++i) {
    const char *token = (argv != nullptr && argv[i] != nullptr) ? argv[i] : "";
    utf16_argv.push_back(UTF8ToUTF16(token));
  }

  ParseFromUTF16Argv(utf16_argv);
}

void CommandLine::ParseFromUTF16Argv(const std::vector<std::u16string> &argv) {
  impl_->argv = argv;
  impl_->switches.clear();
  impl_->args.clear();
  MarkFullArgvDirty(impl_.get());

  bool parse_switches = true;
  for (std::size_t i = 1; i < impl_->argv.size(); ++i) {
    const std::u16string &token = impl_->argv[i];

    if (parse_switches && token == u"--") {
      parse_switches = false;
      continue;
    }

    std::u16string switch_name;
    std::u16string switch_value;
    if (parse_switches && ParseSwitchToken(token, &switch_name, &switch_value)) {
      impl_->switches[switch_name] = switch_value;
      continue;
    }

    impl_->args.push_back(token);
  }
}

} // namespace nei
