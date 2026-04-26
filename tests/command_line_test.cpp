#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <neixx/command_line/command_line.h>

TEST(CommandLineTest, ParsesPosixStyleSwitchesAndArgs) {
  const char *argv[] = {"prog", "--test", "--name=value", "input.txt", "--", "--literal"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  EXPECT_EQ(command_line.GetProgram(), "prog");
  EXPECT_EQ(command_line.GetProgramUTF16(), u"prog");
  EXPECT_TRUE(command_line.HasSwitch("test"));
  EXPECT_TRUE(command_line.HasSwitch(u"test"));
  EXPECT_EQ(command_line.GetSwitchValueASCII("name"), "value");
  EXPECT_EQ(command_line.GetSwitchValueASCII(u"name"), "value");
  EXPECT_EQ(command_line.GetSwitchValueUTF16("name"), u"value");
  EXPECT_EQ(command_line.GetSwitchValueUTF16(u"name"), u"value");

  const std::vector<std::string> args = command_line.GetArgs();
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "input.txt");
  EXPECT_EQ(args[1], "--literal");

  const nei::CommandLine::StringVector &raw_argv = command_line.GetRawArgv();
  const nei::CommandLine::StringVector &full_argv = command_line.GetFullArgv();
  ASSERT_EQ(raw_argv.size(), 6u);
  ASSERT_EQ(full_argv.size(), 6u);
  EXPECT_EQ(raw_argv[0], u"prog");
  EXPECT_EQ(full_argv[0], u"prog");
}

TEST(CommandLineTest, MissingProgramAndMissingSwitchValueReturnEmpty) {
  nei::CommandLine command_line(0, nullptr);

  EXPECT_TRUE(command_line.GetProgram().empty());
  EXPECT_TRUE(command_line.GetProgramUTF16().empty());
  EXPECT_TRUE(command_line.GetSwitchValueASCII("missing").empty());
  EXPECT_TRUE(command_line.GetSwitchValueUTF16("missing").empty());
}

TEST(CommandLineTest, AppendSwitchInsertsBeforeArgs) {
  const char *argv[] = {"prog", "first_arg"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  command_line.AppendSwitchASCII("name", "value");

  EXPECT_EQ(command_line.GetCommandLineString(), "prog --name=value first_arg");
  ASSERT_EQ(command_line.argv().size(), 3u);
  EXPECT_EQ(command_line.argv()[0], u"prog");
  EXPECT_EQ(command_line.argv()[1], u"--name=value");
  EXPECT_EQ(command_line.argv()[2], u"first_arg");
}

TEST(CommandLineTest, AppendsAndQuotesArguments) {
  const char *argv[] = {"prog"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  command_line.AppendSwitch("test");
  command_line.AppendArg("C:/tmp/a b.txt");

  EXPECT_TRUE(command_line.HasSwitch("test"));
  EXPECT_EQ(command_line.GetCommandLineString(), "prog --test \"C:/tmp/a b.txt\"");
}

TEST(CommandLineTest, TerminatorStopsSwitchParsing) {
  const char *argv[] = {"prog", "--flag", "--", "--not-a-switch", "tail"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  EXPECT_TRUE(command_line.HasSwitch("flag"));
  EXPECT_FALSE(command_line.HasSwitch("not-a-switch"));

  const std::vector<std::string> args = command_line.GetArgs();
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "--not-a-switch");
  EXPECT_EQ(args[1], "tail");
}

TEST(CommandLineTest, AppendSwitchUTF16AndGetSwitchesExposeReadOnlyViews) {
  const char *argv[] = {"prog", "arg1"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);
  const std::u16string expected_path = u"C:/\u6d4b\u8bd5/\u6587\u4ef6.txt";

  command_line.AppendSwitchUTF16(u"Path", expected_path);

  EXPECT_TRUE(command_line.HasSwitch("path"));
  EXPECT_EQ(command_line.GetSwitchValueUTF16("path"), expected_path);

  const nei::CommandLine::SwitchMap &switches = command_line.GetSwitches();
  ASSERT_EQ(switches.size(), 1u);
  const auto it = switches.find(u"path");
  ASSERT_NE(it, switches.end());
  EXPECT_EQ(it->second, expected_path);

  const nei::CommandLine::StringVector &argv_view = command_line.argv();
  ASSERT_EQ(argv_view.size(), 3u);
  EXPECT_EQ(argv_view[0], u"prog");
  EXPECT_EQ(argv_view[1], std::u16string(u"--path=") + expected_path);
  EXPECT_EQ(argv_view[2], u"arg1");
}

TEST(CommandLineTest, EmptyValueSwitchRemainsPresentWithoutSeparator) {
  const char *argv[] = {"prog"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  command_line.AppendSwitchASCII("empty", "");

  EXPECT_TRUE(command_line.HasSwitch("empty"));
  EXPECT_TRUE(command_line.GetSwitchValueASCII("empty").empty());
  EXPECT_EQ(command_line.GetCommandLineString(), "prog --empty");
}

TEST(CommandLineTest, GetArgsUTF16ExposesPositionalArguments) {
  const char *argv[] = {"prog", "--flag", "input.txt", "--", "--literal"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  const nei::CommandLine::StringVector &args = command_line.GetArgsUTF16();
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], u"input.txt");
  EXPECT_EQ(args[1], u"--literal");
}

TEST(CommandLineTest, CopySwitchesFromOverridesExistingSwitchToken) {
  const char *child_argv[] = {"child", "--name=old", "child_arg"};
  const char *parent_argv[] = {"parent", "--flag", "--name=new", "parent_arg"};
  nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);
  nei::CommandLine parent(static_cast<int>(std::size(parent_argv)), parent_argv);

  child.CopySwitchesFrom(parent);

  EXPECT_TRUE(child.HasSwitch("flag"));
  EXPECT_EQ(child.GetSwitchValueASCII("name"), "new");
  EXPECT_EQ(child.GetCommandLineString(), "child --flag --name=new child_arg");
}

TEST(CommandLineTest, CopySwitchesFromSupportsWhitelist) {
  const char *child_argv[] = {"child"};
  const char *parent_argv[] = {"parent", "--keep=yes", "--drop=no"};
  nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);
  nei::CommandLine parent(static_cast<int>(std::size(parent_argv)), parent_argv);

  child.CopySwitchesFrom(parent, {"keep"});

  EXPECT_TRUE(child.HasSwitch("keep"));
  EXPECT_FALSE(child.HasSwitch("drop"));
  EXPECT_EQ(child.GetCommandLineString(), "child --keep=yes");
}

TEST(CommandLineTest, CopySwitchesFromOptionsSupportsValuesOnlyAndBlacklist) {
  const char *child_argv[] = {"child"};
  const char *parent_argv[] = {"parent", "--keep=yes", "--novalue", "--secret=token", "--other=1"};
  nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);
  nei::CommandLine parent(static_cast<int>(std::size(parent_argv)), parent_argv);
  nei::CommandLine::CopySwitchesOptions options;
  options.value_filter = nei::CommandLine::SwitchValueFilter::kWithValueOnly;
  options.blacklist = {"secret"};

  child.CopySwitchesFrom(parent, options);

  EXPECT_TRUE(child.HasSwitch("keep"));
  EXPECT_TRUE(child.HasSwitch("other"));
  EXPECT_FALSE(child.HasSwitch("novalue"));
  EXPECT_FALSE(child.HasSwitch("secret"));
  EXPECT_EQ(child.GetCommandLineString(), "child --keep=yes --other=1");
}

TEST(CommandLineTest, CopySwitchesFromOptionsSupportsBooleanSwitchesOnly) {
  const char *child_argv[] = {"child"};
  const char *parent_argv[] = {"parent", "--flag", "--name=value", "--verbose"};
  nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);
  nei::CommandLine parent(static_cast<int>(std::size(parent_argv)), parent_argv);
  nei::CommandLine::CopySwitchesOptions options;
  options.value_filter = nei::CommandLine::SwitchValueFilter::kWithoutValueOnly;

  child.CopySwitchesFrom(parent, options);

  EXPECT_TRUE(child.HasSwitch("flag"));
  EXPECT_TRUE(child.HasSwitch("verbose"));
  EXPECT_FALSE(child.HasSwitch("name"));
  EXPECT_EQ(child.GetCommandLineString(), "child --flag --verbose");
}

TEST(CommandLineTest, PreserveExistingDuplicateSwitchPolicyKeepsOriginalValue) {
  const char *argv[] = {"prog", "--name=old"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  command_line.AppendSwitchASCII("name", "new", nei::CommandLine::DuplicateSwitchPolicy::kPreserveExisting);

  EXPECT_EQ(command_line.GetSwitchValueASCII("name"), "old");
  EXPECT_EQ(command_line.GetCommandLineString(), "prog --name=old");
}

TEST(CommandLineTest, AppendArgumentsMergesTokensForSubprocessAssembly) {
  const char *child_argv[] = {"child", "--child-flag"};
  const char *parent_argv[] = {"parent", "--parent-flag", "input.txt", "--", "--literal"};
  nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);
  nei::CommandLine parent(static_cast<int>(std::size(parent_argv)), parent_argv);

  child.AppendArguments(parent, false);

  EXPECT_TRUE(child.HasSwitch("child-flag"));
  EXPECT_TRUE(child.HasSwitch("parent-flag"));

  const std::vector<std::string> args = child.GetArgs();
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "input.txt");
  EXPECT_EQ(args[1], "--literal");
}

TEST(CommandLineTest, PrependWrapperAffectsFullCommandLineButPreservesChildSemantics) {
  const char *child_argv[] = {"child", "--child-flag", "arg1"};
  const char *wrapper_argv[] = {"sandbox", "--trace"};
  nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);
  nei::CommandLine wrapper(static_cast<int>(std::size(wrapper_argv)), wrapper_argv);

  child.PrependWrapper(wrapper);

  EXPECT_EQ(child.GetProgram(), "child");
  EXPECT_TRUE(child.HasSwitch("child-flag"));
  EXPECT_EQ(child.GetCommandLineString(), "sandbox --trace child --child-flag arg1");

  const nei::CommandLine::StringVector &raw_argv = child.GetRawArgv();
  const nei::CommandLine::StringVector &full_argv = child.GetFullArgv();
  ASSERT_EQ(raw_argv.size(), 3u);
  EXPECT_EQ(raw_argv[0], u"child");
  EXPECT_EQ(raw_argv[1], u"--child-flag");
  EXPECT_EQ(raw_argv[2], u"arg1");
  const nei::CommandLine::StringVector &wrapper_view = child.GetWrapperArgv();
  ASSERT_EQ(wrapper_view.size(), 2u);
  EXPECT_EQ(wrapper_view[0], u"sandbox");
  EXPECT_EQ(wrapper_view[1], u"--trace");
  ASSERT_EQ(full_argv.size(), 5u);
  EXPECT_EQ(full_argv[0], u"sandbox");
  EXPECT_EQ(full_argv[1], u"--trace");
  EXPECT_EQ(full_argv[2], u"child");
  EXPECT_EQ(full_argv[3], u"--child-flag");
  EXPECT_EQ(full_argv[4], u"arg1");
  EXPECT_EQ(child.argv(), full_argv);
}

TEST(CommandLineTest, PrependWrapperStringOverloadAddsWrapperTokens) {
  const char *child_argv[] = {"child", "arg1"};
  nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);

  child.PrependWrapper("launcher", {"--mode=fast", "--trace"});

  EXPECT_EQ(child.GetCommandLineString(), "launcher --mode=fast --trace child arg1");
}

TEST(CommandLineTest, PrependWrapperUTF16OverloadAddsWrapperTokens) {
  const char *child_argv[] = {"child"};
  nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);
  const nei::CommandLine::StringVector wrapper_args = {u"--profile", u"C:/\u6d4b\u8bd5"};

  child.PrependWrapperUTF16(u"sandbox", wrapper_args);

  const nei::CommandLine::StringVector &argv_view = child.argv();
  ASSERT_EQ(argv_view.size(), 4u);
  EXPECT_EQ(argv_view[0], u"sandbox");
  EXPECT_EQ(argv_view[1], u"--profile");
  EXPECT_EQ(argv_view[2], u"C:/\u6d4b\u8bd5");
  EXPECT_EQ(argv_view[3], u"child");
}

#if defined(_WIN32)
TEST(CommandLineTest, ParsesWindowsSlashAndColonSwitches) {
  const char *argv[] = {"prog.exe", "/test", "/name:value", "tail"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  EXPECT_TRUE(command_line.HasSwitch("test"));
  EXPECT_EQ(command_line.GetSwitchValueASCII("name"), "value");

  const std::vector<std::string> args = command_line.GetArgs();
  ASSERT_EQ(args.size(), 1u);
  EXPECT_EQ(args[0], "tail");
}
#else
TEST(CommandLineTest, PosixTreatsSlashTokenAsArgument) {
  const char *argv[] = {"prog", "/not-a-switch", "tail"};
  nei::CommandLine command_line(static_cast<int>(std::size(argv)), argv);

  EXPECT_FALSE(command_line.HasSwitch("not-a-switch"));

  const std::vector<std::string> args = command_line.GetArgs();
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "/not-a-switch");
  EXPECT_EQ(args[1], "tail");
}
#endif