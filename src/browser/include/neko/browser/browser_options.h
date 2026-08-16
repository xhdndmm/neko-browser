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
  // --profile <dir>: browser profile directory.
  std::optional<std::string> profile_name;
  // --disable-gpu: force software rendering.
  bool disable_gpu = false;
  // --verbose / --log-level <level>.
  base::LogLevel log_level = base::LogLevel::kInfo;

  // Storage / content commands.
  bool dump_history = false;    // print the browsing history
  bool dump_bookmarks = false;  // print bookmarks
  bool show_cookies = false;    // print stored cookies
  std::optional<std::string> download_url;     // --download <url>
  std::optional<std::string> download_dir;     // --download-dir <dir>
  std::optional<std::string> extract_pdf_path; // --extract-pdf <file>
  // PDF page rendering (with --extract-pdf): write the rasterized page as a
  // binary PPM.
  std::optional<std::string> pdf_render_out; // --pdf-render-out <path>
  int pdf_page = 0;                          // --pdf-page <n> (0-based)
  float pdf_scale = 1.0f;                    // --pdf-scale <f> (1 = 72 dpi)
  std::optional<std::string> audio_info_path;  // --audio-info <file>
  std::optional<std::string> image_info_path;  // --image-info <file>
  std::optional<std::string> image_out_ppm;    // --image-out <path>
  // --eval <script>: evaluate a JavaScript expression (QuickJS runtime).
  std::optional<std::string> eval_script;
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
