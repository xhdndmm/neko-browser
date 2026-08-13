#include "neko/browser/browser_options.h"

#include <string>
#include <string_view>
#include <vector>

namespace neko::browser {

std::string UsageText() {
  return "Usage: neko-browser [options] [url]\n"
         "\n"
         "A from-scratch, cross-platform browser engine (Phase 0 bootstrap).\n"
         "\n"
         "Options:\n"
         "  -h, --help                 Show this help and exit.\n"
         "  -v, --version              Print version information and exit.\n"
         "      --headless             Run without a GUI (engine-only mode).\n"
         "      --url <url>            URL to navigate to.\n"
         "      --dump-dom             Dump the DOM after load (requires Phase 3).\n"
         "      --screenshot <path>    Render a screenshot to <path> (requires Phase 6).\n"
         "      --profile <name>       Use the named browser profile.\n"
         "      --disable-gpu          Force software rendering.\n"
         "      --verbose              Enable debug logging.\n"
         "      --log-level <level>    One of trace, debug, info, warning, error, fatal.\n"
         "\n"
         "Phase 0 status: navigation, rendering and the GUI are NOT IMPLEMENTED yet.\n"
         "This executable currently validates the CLI, logging and build pipeline.\n";
}

ParseResult ParseCommandLine(int argc, char** argv) {
  ParseResult result;
  const std::vector<std::string_view> args(argv + 1, argv + argc);

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];

    if (arg == "-h" || arg == "--help") {
      result.action = ParseResult::Action::kHelp;
      return result;
    }
    if (arg == "-v" || arg == "--version") {
      result.action = ParseResult::Action::kVersion;
      return result;
    }
    if (arg == "--headless") {
      result.options.headless = true;
      continue;
    }
    if (arg == "--dump-dom") {
      result.options.dump_dom = true;
      continue;
    }
    if (arg == "--disable-gpu") {
      result.options.disable_gpu = true;
      continue;
    }
    if (arg == "--verbose") {
      result.options.log_level = base::LogLevel::kDebug;
      continue;
    }
    if (arg == "--url") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--url' requires an argument";
        return result;
      }
      result.options.url = std::string(args[++i]);
      continue;
    }
    if (arg == "--screenshot") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--screenshot' requires an argument";
        return result;
      }
      result.options.screenshot_path = std::string(args[++i]);
      continue;
    }
    if (arg == "--profile") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--profile' requires an argument";
        return result;
      }
      result.options.profile_name = std::string(args[++i]);
      continue;
    }
    if (arg == "--log-level") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--log-level' requires an argument";
        return result;
      }
      base::LogLevel level = base::LogLevel::kInfo;
      if (!base::ParseLogLevel(args[++i], level)) {
        result.action = ParseResult::Action::kError;
        result.error_message = "invalid log level: " + std::string(args[i]);
        return result;
      }
      result.options.log_level = level;
      continue;
    }
    if (arg == "--") {
      // Everything after "--" is treated as a positional URL.
      if (i + 1 < args.size()) {
        result.options.url = std::string(args[i + 1]);
      }
      return result;
    }
    if (!arg.empty() && arg[0] == '-') {
      result.action = ParseResult::Action::kError;
      result.error_message = "unknown option: " + std::string(arg);
      return result;
    }
    if (!result.options.url.has_value()) {
      result.options.url = std::string(arg);
    } else {
      result.action = ParseResult::Action::kError;
      result.error_message = "unexpected extra argument: " + std::string(arg);
      return result;
    }
  }

  return result;
}

}  // namespace neko::browser
