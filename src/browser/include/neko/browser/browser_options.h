#pragma once

#include <optional>
#include <string>

#include "neko/base/logging.h"

namespace neko::browser {

// Parsed command-line options for the browser executable.
//
// Phase 0 honors the flags syntactically; navigation/rendering options are
// reported as NOT IMPLEMENTED by main() until their owning phases land.
struct BrowserOptions {
  // URL requested via --url or as a positional argument.
  std::optional<std::string> url;
  // --headless: run without a GUI (engine-only mode).
  bool headless = false;
  // --dump-dom: print the DOM after load (requires Phase 3).
  bool dump_dom = false;
  // --screenshot <path>: render and write a screenshot (requires Phase 6).
  std::optional<std::string> screenshot_path;
  // --profile <name>: browser profile to use.
  std::optional<std::string> profile_name;
  // --disable-gpu: force software rendering.
  bool disable_gpu = false;
  // --verbose / --log-level <level>.
  base::LogLevel log_level = base::LogLevel::kInfo;
};

struct ParseResult {
  enum class Action { kRun, kHelp, kVersion, kError };

  Action action = Action::kRun;
  std::string error_message;
  BrowserOptions options;
};

// Parses argc/argv.  Never throws; failures are reported via Action::kError.
ParseResult ParseCommandLine(int argc, char** argv);

std::string UsageText();

}  // namespace neko::browser
