#pragma once

#include <atomic>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/macros.h"

namespace neko::base {

// ---------------------------------------------------------------------------
// Log levels
// ---------------------------------------------------------------------------
enum class LogLevel : int {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarning = 3,
  kError = 4,
  kFatal = 5,
};

// Short human-readable name, e.g. "TRACE", "INFO".
std::string_view ToString(LogLevel level);

// Parses a case-insensitive level name ("trace", "DEBUG", "Info", ...).
// Returns false for unknown names and leaves |out| untouched.
bool ParseLogLevel(std::string_view name, LogLevel& out);

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------
// A LogSink receives already-formatted, newline-terminated log lines.
class LogSink {
 public:
  virtual ~LogSink() = default;
  virtual void Write(LogLevel level, std::string_view message) = 0;
};

// Writes (optionally colorized) log lines to the process stderr.
class ConsoleLogSink final : public LogSink {
 public:
  explicit ConsoleLogSink(bool use_color = true);
  void Write(LogLevel level, std::string_view message) override;

 private:
  bool use_color_;
};

// Appends log lines to a file, truncating it on construction.
class FileLogSink final : public LogSink {
 public:
  explicit FileLogSink(std::string_view path);
  ~FileLogSink() override;

  NEKO_DISALLOW_COPY_AND_MOVE(FileLogSink)

  void Write(LogLevel level, std::string_view message) override;

 private:
  std::string path_;
  std::ofstream file_;
};

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
// Process-wide, thread-safe logger.  Access via Logger::Instance() or the
// NEKO_LOG* macros.  If no sink has been registered, the first log call
// attaches a ConsoleLogSink automatically.
class Logger {
 public:
  static Logger& Instance();

  NEKO_DISALLOW_COPY_AND_MOVE(Logger)

  void SetLevel(LogLevel level);
  LogLevel level() const;
  bool IsEnabled(LogLevel level) const;

  void AddSink(std::unique_ptr<LogSink> sink);

  // Logs a fully formatted line.  Thread-safe; used by the NEKO_LOG* macros.
  void Log(LogLevel level, std::string_view file, int line, std::string_view message);

  // Logs a bare message (no source location decoration).
  void Log(LogLevel level, std::string_view message);

 private:
  Logger() = default;

  void WriteToSinks(LogLevel level, const std::string& message);

  std::mutex mutex_;
  std::atomic<int> level_{static_cast<int>(LogLevel::kInfo)};
  std::vector<std::unique_ptr<LogSink>> sinks_;
};

}  // namespace neko::base

// ---------------------------------------------------------------------------
// Logging macros
// ---------------------------------------------------------------------------
#define NEKO_LOG(level, message)                                          \
  do {                                                                    \
    if (::neko::base::Logger::Instance().IsEnabled(level)) {              \
      ::neko::base::Logger::Instance().Log(level, __FILE__, __LINE__,     \
                                           (message));                    \
    }                                                                     \
  } while (false)

// printf-style variant using std::format.
#define NEKO_LOGF(level, format, ...)                                     \
  do {                                                                    \
    if (::neko::base::Logger::Instance().IsEnabled(level)) {              \
      ::neko::base::Logger::Instance().Log(                               \
          level, __FILE__, __LINE__, ::std::format((format), __VA_ARGS__)); \
    }                                                                     \
  } while (false)

#define NEKO_LOG_TRACE(message) NEKO_LOG(::neko::base::LogLevel::kTrace, message)
#define NEKO_LOG_DEBUG(message) NEKO_LOG(::neko::base::LogLevel::kDebug, message)
#define NEKO_LOG_INFO(message) NEKO_LOG(::neko::base::LogLevel::kInfo, message)
#define NEKO_LOG_WARNING(message) NEKO_LOG(::neko::base::LogLevel::kWarning, message)
#define NEKO_LOG_ERROR(message) NEKO_LOG(::neko::base::LogLevel::kError, message)
#define NEKO_LOG_FATAL(message) NEKO_LOG(::neko::base::LogLevel::kFatal, message)

#define NEKO_LOG_TRACE_F(format, ...) \
  NEKO_LOGF(::neko::base::LogLevel::kTrace, format, __VA_ARGS__)
#define NEKO_LOG_DEBUG_F(format, ...) \
  NEKO_LOGF(::neko::base::LogLevel::kDebug, format, __VA_ARGS__)
#define NEKO_LOG_INFO_F(format, ...) \
  NEKO_LOGF(::neko::base::LogLevel::kInfo, format, __VA_ARGS__)
#define NEKO_LOG_WARNING_F(format, ...) \
  NEKO_LOGF(::neko::base::LogLevel::kWarning, format, __VA_ARGS__)
#define NEKO_LOG_ERROR_F(format, ...) \
  NEKO_LOGF(::neko::base::LogLevel::kError, format, __VA_ARGS__)
