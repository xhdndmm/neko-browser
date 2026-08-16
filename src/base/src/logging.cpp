#include "neko/base/logging.h"

#include "neko/base/string_util.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <format>
#include <iostream>
#include <string>

namespace neko::base {
namespace {

std::string_view Basename(std::string_view path)
{
  const std::size_t pos = path.find_last_of("/\\");
  if (pos == std::string_view::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

const char* ColorFor(LogLevel level)
{
  switch (level) {
  case LogLevel::kTrace:
    return "\033[90m";
  case LogLevel::kDebug:
    return "\033[34m";
  case LogLevel::kInfo:
    return "\033[32m";
  case LogLevel::kWarning:
    return "\033[33m";
  case LogLevel::kError:
    return "\033[31m";
  case LogLevel::kFatal:
    return "\033[1;31m";
  }
  return "";
}

std::string FormatLogLine(LogLevel level, std::string_view file, int line, std::string_view message)
{
  const auto now = std::chrono::system_clock::now();
  const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &timestamp);
#else
  localtime_r(&timestamp, &local);
#endif

  char time_buf[16] = {};
  std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &local);

  return std::format("[{}.{:03d}] [{:<5}] {}:{} {}",
                     time_buf,
                     millis,
                     ToString(level),
                     Basename(file),
                     line,
                     message);
}

} // namespace

std::string_view ToString(LogLevel level)
{
  switch (level) {
  case LogLevel::kTrace:
    return "TRACE";
  case LogLevel::kDebug:
    return "DEBUG";
  case LogLevel::kInfo:
    return "INFO";
  case LogLevel::kWarning:
    return "WARN";
  case LogLevel::kError:
    return "ERROR";
  case LogLevel::kFatal:
    return "FATAL";
  }
  return "UNKNOWN";
}

bool ParseLogLevel(std::string_view name, LogLevel& out)
{
  struct Entry
  {
    std::string_view name;
    LogLevel level;
  };
  static constexpr Entry kEntries[] = {
      {"trace", LogLevel::kTrace},
      {"debug", LogLevel::kDebug},
      {"info", LogLevel::kInfo},
      {"warning", LogLevel::kWarning},
      {"warn", LogLevel::kWarning},
      {"error", LogLevel::kError},
      {"fatal", LogLevel::kFatal},
  };
  for (const Entry& entry : kEntries) {
    if (AsciiEqualsIgnoreCase(name, entry.name)) {
      out = entry.level;
      return true;
    }
  }
  return false;
}

ConsoleLogSink::ConsoleLogSink(bool use_color) : use_color_(use_color) {}

void ConsoleLogSink::Write(LogLevel level, std::string_view message)
{
  if (use_color_) {
    std::cerr << ColorFor(level) << message << "\033[0m\n";
  } else {
    std::cerr << message << '\n';
  }
}

FileLogSink::FileLogSink(std::string_view path)
    : path_(path), file_(std::string(path), std::ios::out | std::ios::trunc)
{}

FileLogSink::~FileLogSink() = default;

void FileLogSink::Write(LogLevel level, std::string_view message)
{
  (void)level;
  if (file_.is_open()) {
    file_ << message << '\n';
    file_.flush();
  } else {
    std::cerr << "[file-sink] cannot write to log file: " << path_ << '\n';
  }
}

Logger& Logger::Instance()
{
  static Logger instance;
  return instance;
}

void Logger::SetLevel(LogLevel level)
{
  level_.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel Logger::level() const
{
  return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
}

bool Logger::IsEnabled(LogLevel level) const
{
  return static_cast<int>(level) >= level_.load(std::memory_order_relaxed);
}

void Logger::AddSink(std::unique_ptr<LogSink> sink)
{
  std::lock_guard<std::mutex> lock(mutex_);
  sinks_.push_back(std::move(sink));
}

void Logger::Log(LogLevel level, std::string_view file, int line, std::string_view message)
{
  if (!IsEnabled(level)) {
    return;
  }
  WriteToSinks(level, FormatLogLine(level, file, line, message));
}

void Logger::Log(LogLevel level, std::string_view message)
{
  if (!IsEnabled(level)) {
    return;
  }
  WriteToSinks(level, std::string(message));
}

void Logger::WriteToSinks(LogLevel level, const std::string& message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (sinks_.empty()) {
    sinks_.push_back(std::make_unique<ConsoleLogSink>());
  }
  for (const auto& sink : sinks_) {
    sink->Write(level, message);
  }
  if (level == LogLevel::kFatal) {
    std::abort();
  }
}

} // namespace neko::base
