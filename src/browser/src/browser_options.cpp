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
         "      --dump-history         Print the browsing history.\n"
         "      --dump-bookmarks       Print the bookmarks.\n"
         "      --show-cookies         Print the stored cookies.\n"
         "      --download <url>       Download a URL to the download directory.\n"
         "      --download-dir <dir>   Download directory (default: <profile>/downloads).\n"
         "      --extract-pdf <file>   Extract text from a PDF; with --pdf-render-out,\n"
         "                            rasterize a page to PPM.\n"
         "      --audio-info <file>    Print WAV metadata.\n"
         "      --image-info <file>    Decode an image; optionally write PPM via --image-out.\n"      "      --video-info <file>    Decode a video (FFmpeg); print container/codec/metadata.\n"
      "      --video-out <file>     With --video-info: write the first frame as PPM.\n"         "      --eval <script>         Evaluate a JavaScript expression.\n"
         "      --profile <dir>        Browser profile directory.\n"
         "      --disable-gpu          Force software rendering.\n"
         "      --verbose              Enable debug logging.\n"
         "      --log-level <level>    One of trace, debug, info, warning, error, fatal.\n"
         "\n"
         "Phase 6 status: http:// fetching, HTML parsing, styling, layout and\n"
         "software rendering are implemented; the GUI is not yet available.\n";
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
    if (arg == "--dump-history") {
      result.options.dump_history = true;
      continue;
    }
    if (arg == "--dump-bookmarks") {
      result.options.dump_bookmarks = true;
      continue;
    }
    if (arg == "--show-cookies") {
      result.options.show_cookies = true;
      continue;
    }
    if (arg == "--download") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--download' requires an argument";
        return result;
      }
      result.options.download_url = std::string(args[++i]);
      continue;
    }
    if (arg == "--download-dir") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--download-dir' requires an argument";
        return result;
      }
      result.options.download_dir = std::string(args[++i]);
      continue;
    }
    if (arg == "--extract-pdf") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--extract-pdf' requires an argument";
        return result;
      }
      result.options.extract_pdf_path = std::string(args[++i]);
      continue;
    }
    if (arg == "--pdf-render-out") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--pdf-render-out' requires an argument";
        return result;
      }
      result.options.pdf_render_out = std::string(args[++i]);
      continue;
    }
    if (arg == "--pdf-page") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--pdf-page' requires an argument";
        return result;
      }
      result.options.pdf_page = std::atoi(std::string(args[++i]).c_str());
      continue;
    }
    if (arg == "--pdf-scale") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--pdf-scale' requires an argument";
        return result;
      }
      result.options.pdf_scale =
          static_cast<float>(std::atof(std::string(args[++i]).c_str()));
      continue;
    }
    if (arg == "--audio-info") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--audio-info' requires an argument";
        return result;
      }
      result.options.audio_info_path = std::string(args[++i]);
      continue;
    }
    if (arg == "--image-info") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--image-info' requires an argument";
        return result;
      }
      result.options.image_info_path = std::string(args[++i]);
      continue;
    }
    if (arg == "--image-out") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--image-out' requires an argument";
        return result;
      }
      result.options.image_out_ppm = std::string(args[++i]);
      continue;
    }
    if (arg == "--video-info") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--video-info' requires an argument";
        return result;
      }
      result.options.video_info_path = std::string(args[++i]);
      continue;
    }
    if (arg == "--video-out") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--video-out' requires an argument";
        return result;
      }
      result.options.video_out_ppm = std::string(args[++i]);
      continue;
    }
    if (arg == "--eval") {
      if (i + 1 >= args.size()) {
        result.action = ParseResult::Action::kError;
        result.error_message = "option '--eval' requires an argument";
        return result;
      }
      result.options.eval_script = std::string(args[++i]);
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
