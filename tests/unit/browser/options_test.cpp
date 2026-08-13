#include "neko/browser/browser_options.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace neko::browser {
namespace {

inline constexpr char kProgramName[] = "neko-browser";

// Builds a real argv layout: argv[0] is the program name and argc includes it,
// exactly like main() receives it.
std::vector<char*> ToArgv(std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  argv.push_back(const_cast<char*>(kProgramName));
  for (std::string& s : args) {
    argv.push_back(s.data());
  }
  return argv;
}

ParseResult Parse(std::vector<std::string> args) {
  std::vector<char*> argv = ToArgv(args);
  return ParseCommandLine(static_cast<int>(argv.size()), argv.data());
}

TEST(OptionsTest, VersionAction) {
  const ParseResult r = Parse({"--version"});
  EXPECT_EQ(r.action, ParseResult::Action::kVersion);
}

TEST(OptionsTest, VersionShortFlag) {
  const ParseResult r = Parse({"-v"});
  EXPECT_EQ(r.action, ParseResult::Action::kVersion);
}

TEST(OptionsTest, HelpAction) {
  const ParseResult r = Parse({"--help"});
  EXPECT_EQ(r.action, ParseResult::Action::kHelp);
}

TEST(OptionsTest, HelpShortFlag) {
  const ParseResult r = Parse({"-h"});
  EXPECT_EQ(r.action, ParseResult::Action::kHelp);
}

TEST(OptionsTest, UrlFlag) {
  const ParseResult r = Parse({"--url", "https://example.com"});
  EXPECT_EQ(r.action, ParseResult::Action::kRun);
  ASSERT_TRUE(r.options.url.has_value());
  EXPECT_EQ(r.options.url.value(), "https://example.com");
}

TEST(OptionsTest, PositionalUrl) {
  const ParseResult r = Parse({"https://example.com"});
  EXPECT_EQ(r.action, ParseResult::Action::kRun);
  ASSERT_TRUE(r.options.url.has_value());
  EXPECT_EQ(r.options.url.value(), "https://example.com");
}

TEST(OptionsTest, DoubleDashPrefixUrl) {
  const ParseResult r = Parse({"--", "https://example.com"});
  EXPECT_EQ(r.action, ParseResult::Action::kRun);
  ASSERT_TRUE(r.options.url.has_value());
  EXPECT_EQ(r.options.url.value(), "https://example.com");
}

TEST(OptionsTest, HeadlessFlag) {
  const ParseResult r = Parse({"--headless"});
  EXPECT_TRUE(r.options.headless);
}

TEST(OptionsTest, DumpDomFlag) {
  const ParseResult r = Parse({"--dump-dom"});
  EXPECT_TRUE(r.options.dump_dom);
}

TEST(OptionsTest, DisableGpuFlag) {
  const ParseResult r = Parse({"--disable-gpu"});
  EXPECT_TRUE(r.options.disable_gpu);
}

TEST(OptionsTest, ScreenshotFlag) {
  const ParseResult r = Parse({"--screenshot", "/tmp/out.png"});
  ASSERT_TRUE(r.options.screenshot_path.has_value());
  EXPECT_EQ(r.options.screenshot_path.value(), "/tmp/out.png");
}

TEST(OptionsTest, ProfileFlag) {
  const ParseResult r = Parse({"--profile", "default"});
  ASSERT_TRUE(r.options.profile_name.has_value());
  EXPECT_EQ(r.options.profile_name.value(), "default");
}

TEST(OptionsTest, VerboseSetsDebugLevel) {
  const ParseResult r = Parse({"--verbose"});
  EXPECT_EQ(r.options.log_level, neko::base::LogLevel::kDebug);
}

TEST(OptionsTest, LogLevelFlag) {
  const ParseResult r = Parse({"--log-level", "error"});
  EXPECT_EQ(r.options.log_level, neko::base::LogLevel::kError);
}

TEST(OptionsTest, UnknownOptionIsError) {
  const ParseResult r = Parse({"--bogus"});
  EXPECT_EQ(r.action, ParseResult::Action::kError);
  EXPECT_NE(r.error_message.find("unknown option"), std::string::npos);
}

TEST(OptionsTest, MissingUrlArgumentIsError) {
  const ParseResult r = Parse({"--url"});
  EXPECT_EQ(r.action, ParseResult::Action::kError);
  EXPECT_NE(r.error_message.find("--url"), std::string::npos);
}

TEST(OptionsTest, MissingScreenshotArgumentIsError) {
  const ParseResult r = Parse({"--screenshot"});
  EXPECT_EQ(r.action, ParseResult::Action::kError);
}

TEST(OptionsTest, InvalidLogLevelIsError) {
  const ParseResult r = Parse({"--log-level", "loud"});
  EXPECT_EQ(r.action, ParseResult::Action::kError);
  EXPECT_NE(r.error_message.find("invalid log level"), std::string::npos);
}

TEST(OptionsTest, ExtraPositionalIsError) {
  const ParseResult r = Parse({"https://a.example", "https://b.example"});
  EXPECT_EQ(r.action, ParseResult::Action::kError);
  EXPECT_NE(r.error_message.find("extra argument"), std::string::npos);
}

TEST(OptionsTest, EmptyCommandLineRuns) {
  const ParseResult r = Parse({});
  EXPECT_EQ(r.action, ParseResult::Action::kRun);
  EXPECT_FALSE(r.options.url.has_value());
  EXPECT_FALSE(r.options.headless);
}

TEST(OptionsTest, CombinedFlags) {
  const ParseResult r = Parse({"https://example.com", "--headless", "--verbose", "--dump-dom"});
  EXPECT_EQ(r.action, ParseResult::Action::kRun);
  ASSERT_TRUE(r.options.url.has_value());
  EXPECT_EQ(r.options.url.value(), "https://example.com");
  EXPECT_TRUE(r.options.headless);
  EXPECT_TRUE(r.options.dump_dom);
  EXPECT_EQ(r.options.log_level, neko::base::LogLevel::kDebug);
}

TEST(OptionsTest, EvalFlag) {
  const ParseResult r = Parse({"--eval", "1 + 2"});
  EXPECT_EQ(r.action, ParseResult::Action::kRun);
  ASSERT_TRUE(r.options.eval_script.has_value());
  EXPECT_EQ(r.options.eval_script.value(), "1 + 2");
}

TEST(OptionsTest, MissingEvalArgumentIsError) {
  const ParseResult r = Parse({"--eval"});
  EXPECT_EQ(r.action, ParseResult::Action::kError);
}

}  // namespace
}  // namespace neko::browser
