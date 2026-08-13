#include "neko/base/logging.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace neko::base {
namespace {

class CapturingSink final : public LogSink {
 public:
  void Write(LogLevel level, std::string_view message) override {
    levels_.push_back(level);
    messages_.emplace_back(message);
  }

  std::vector<LogLevel> levels_;
  std::vector<std::string> messages_;
};

TEST(LoggingTest, LogLevelToString) {
  EXPECT_EQ(ToString(LogLevel::kTrace), "TRACE");
  EXPECT_EQ(ToString(LogLevel::kDebug), "DEBUG");
  EXPECT_EQ(ToString(LogLevel::kInfo), "INFO");
  EXPECT_EQ(ToString(LogLevel::kWarning), "WARN");
  EXPECT_EQ(ToString(LogLevel::kError), "ERROR");
  EXPECT_EQ(ToString(LogLevel::kFatal), "FATAL");
}

TEST(LoggingTest, ParseLogLevel) {
  LogLevel level = LogLevel::kInfo;
  EXPECT_TRUE(ParseLogLevel("info", level));
  EXPECT_EQ(level, LogLevel::kInfo);
  EXPECT_TRUE(ParseLogLevel("INFO", level));
  EXPECT_EQ(level, LogLevel::kInfo);
  EXPECT_TRUE(ParseLogLevel("Trace", level));
  EXPECT_EQ(level, LogLevel::kTrace);
  EXPECT_TRUE(ParseLogLevel("warning", level));
  EXPECT_EQ(level, LogLevel::kWarning);
  EXPECT_TRUE(ParseLogLevel("warn", level));
  EXPECT_EQ(level, LogLevel::kWarning);
  EXPECT_TRUE(ParseLogLevel("fatal", level));
  EXPECT_EQ(level, LogLevel::kFatal);

  // Unknown name: returns false and leaves |out| untouched.
  const LogLevel before = LogLevel::kError;
  level = before;
  EXPECT_FALSE(ParseLogLevel("bogus", level));
  EXPECT_EQ(level, before);
}

TEST(LoggingTest, LoggerRoutesToSinks) {
  auto sink = std::make_unique<CapturingSink>();
  CapturingSink* raw = sink.get();

  Logger& logger = Logger::Instance();
  logger.AddSink(std::move(sink));
  logger.SetLevel(LogLevel::kTrace);

  logger.Log(LogLevel::kInfo, "plain message");
  logger.Log(LogLevel::kError, "path/to/file.cpp", 42, "decorated message");

  ASSERT_EQ(raw->messages_.size(), 2u);
  EXPECT_EQ(raw->levels_[0], LogLevel::kInfo);
  EXPECT_EQ(raw->messages_[0], "plain message");

  EXPECT_EQ(raw->levels_[1], LogLevel::kError);
  EXPECT_NE(raw->messages_[1].find("file.cpp:42"), std::string::npos);
  EXPECT_NE(raw->messages_[1].find("decorated message"), std::string::npos);
}

TEST(LoggingTest, LevelFiltering) {
  auto sink = std::make_unique<CapturingSink>();
  CapturingSink* raw = sink.get();

  Logger& logger = Logger::Instance();
  logger.AddSink(std::move(sink));
  logger.SetLevel(LogLevel::kWarning);

  logger.Log(LogLevel::kInfo, "dropped");
  logger.Log(LogLevel::kWarning, "kept");

  ASSERT_EQ(raw->messages_.size(), 1u);
  EXPECT_EQ(raw->levels_[0], LogLevel::kWarning);
  EXPECT_EQ(raw->messages_[0], "kept");
}

TEST(LoggingTest, FileSinkWritesLines) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "neko_logging_test";
  fs::create_directories(dir);
  const fs::path file = dir / "test.log";

  {
    FileLogSink sink(file.string());
    sink.Write(LogLevel::kInfo, "line one");
    sink.Write(LogLevel::kError, "line two");
  }

  std::ifstream in(file);
  ASSERT_TRUE(in.is_open());
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("line one"), std::string::npos);
  EXPECT_NE(content.find("line two"), std::string::npos);

  fs::remove_all(dir);
}

}  // namespace
}  // namespace neko::base
