// neko-browser CLI (headless).
//
//  - Page commands:  --url <url|path> [--dump-dom] [--screenshot <path>]
//  - Storage:        --dump-history | --dump-bookmarks | --show-cookies
//  - Content tools:  --download <url> | --extract-pdf <file> |
//                    --audio-info <file> | --image-info <file> [--image-out <path>]
//  - GUI:            run the separate neko_browser_gui executable.

#include "neko/base/logging.h"
#include "neko/base/status.h"
#include "neko/base/version.h"
#include "neko/browser/browser_controller.h"
#include "neko/browser/browser_options.h"
#include "neko/browser/page_scripts.h"
#include "neko/image/image.h"
#include "neko/javascript/script_engine.h"
#include "neko/media/audio.h"
#include "neko/network/http.h"
#include "neko/paint/rasterizer.h"
#include "neko/pdf/pdf.h"
#include "neko/renderer/page.h"
#include "neko/storage/file_util.h"
#include "neko/url/url.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

namespace {

std::string DefaultProfileDir()
{
  const char* env = std::getenv("NEKO_PROFILE");
  if (env != nullptr && *env != '\0')
    return env;
#if defined(_WIN32)
  return std::string(std::getenv("APPDATA") ? std::getenv("APPDATA") : "") + "/neko-browser";
#else
  const char* home = std::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/.local/share/neko-browser";
#endif
}

// Loads a URL (http via the network stack) or a local file into the page.
// Follows page-script navigation requests (window.location) recursively, up to
// a depth cap so a redirect loop terminates.
neko::base::Result<void> LoadTarget(neko::renderer::Page& page,
                                    const std::string& target,
                                    int depth = 0)
{
  constexpr int kMaxNavigationDepth = 20;
  if (depth >= kMaxNavigationDepth) {
    NEKO_LOG_WARNING("navigation chain too deep; stopped at " + target);
    return neko::base::Ok();
  }
  const auto parsed = neko::url::Url::Parse(target);
  if (parsed.has_value()) {
    const neko::url::Url& url = parsed.value();
    if (url.scheme() == "http" || url.scheme() == "https") {
      NEKO_LOG_INFO("fetching " + url.Serialize());
      const auto response = neko::network::HttpGet(url);
      if (!response) {
        return neko::base::Err(response.error());
      }
      NEKO_LOG_INFO("HTTP " + std::to_string(response.value().status_code) + " (" +
                    std::to_string(response.value().body.size()) + " bytes)");
      const auto r = page.LoadHtml(response.value().body);
      if (!r) {
        return r;
      }
      // Fetch and apply external <link rel=stylesheet> sheets before scripts.
      neko::browser::FetchExternalStylesheets(
          page, url.Serialize(), [](const neko::url::Url& u, std::string_view) {
            return neko::network::HttpGet(u);
          });
      // Phase 8 M2: execute the page's scripts (inline + external src=,
      // async/defer); scripts may mutate the DOM and RunPageScripts
      // re-applies styles inside.
      neko::browser::ScriptRequestedNavigation requested;
      neko::browser::RunPageScripts(
          page,
          url.Serialize(),
          [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
          [](std::string_view level, std::string_view text) {
            std::cout << "[" << level << "] " << text << "\n";
          },
          {},
          &requested);
      // A script may have redirected the page (e.g. location.replace()).
      if (!requested.url.empty()) {
        NEKO_LOG_INFO("script navigated to " + requested.url);
        return LoadTarget(page, requested.url, depth + 1);
      }
      if (requested.is_reload) {
        NEKO_LOG_INFO("script reloaded " + url.Serialize());
        return LoadTarget(page, url.Serialize(), depth + 1);
      }
      // Fetch and decode the page's <img> subresources (headless path).
      neko::browser::FetchPageImages(
          page, url.Serialize(), [](const neko::url::Url& u, std::string_view) {
            return neko::network::HttpGet(u);
          });
      return neko::base::Ok();
    }
    if (url.scheme() == "file") {
      const auto r = page.LoadFile(url.path());
      if (!r) {
        return r;
      }
      neko::browser::ScriptRequestedNavigation requested;
      neko::browser::RunPageScripts(
          page,
          "",
          [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
          [](std::string_view level, std::string_view text) {
            std::cout << "[" << level << "] " << text << "\n";
          },
          {},
          &requested);
      if (!requested.url.empty()) {
        NEKO_LOG_INFO("script navigated to " + requested.url);
        return LoadTarget(page, requested.url, depth + 1);
      }
      // Local pages may still reference absolute http(s) images; fetch those.
      neko::browser::FetchPageImages(
          page, /*base_url=*/"", [](const neko::url::Url& u, std::string_view) {
            return neko::network::HttpGet(u);
          });
      return neko::base::Ok();
    }
    return neko::base::Err(
        neko::base::Error::NotImplemented("unsupported URL scheme: " + url.scheme()));
  }
  const auto r = page.LoadFile(target);
  if (!r) {
    return r;
  }
  neko::browser::ScriptRequestedNavigation requested;
  neko::browser::RunPageScripts(
      page,
      "",
      [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
      [](std::string_view level, std::string_view text) {
        std::cout << "[" << level << "] " << text << "\n";
      },
      {},
      &requested);
  if (!requested.url.empty()) {
    NEKO_LOG_INFO("script navigated to " + requested.url);
    return LoadTarget(page, requested.url, depth + 1);
  }
  return neko::base::Ok();
}

// Writes an image::Image as a binary PPM (P6), compositing alpha over white.
neko::base::Result<void> WriteImagePpm(std::string_view path, const neko::image::Image& img)
{
  if (img.empty()) {
    return neko::base::Err(neko::base::Error::InvalidArgument("empty image"));
  }
  std::string out =
      "P6\n" + std::to_string(img.width) + " " + std::to_string(img.height) + "\n255\n";
  out.reserve(out.size() + static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 3);
  for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
    const uint8_t r = img.rgba[i], g = img.rgba[i + 1], b = img.rgba[i + 2], a = img.rgba[i + 3];
    // Composite over white.
    const uint8_t cr = static_cast<uint8_t>((r * a + 255 * (255 - a)) / 255);
    const uint8_t cg = static_cast<uint8_t>((g * a + 255 * (255 - a)) / 255);
    const uint8_t cb = static_cast<uint8_t>((b * a + 255 * (255 - a)) / 255);
    out.push_back(static_cast<char>(cr));
    out.push_back(static_cast<char>(cg));
    out.push_back(static_cast<char>(cb));
  }
  return neko::storage::WriteFileAtomic(path, out);
}

int RunStorageCommands(const neko::browser::BrowserOptions& options, const std::string& profile_dir)
{
  neko::browser::BrowserController controller(
      profile_dir, [](const neko::url::Url&, std::string_view) {
        return neko::base::Err(
            neko::base::Error::NotImplemented("network disabled in storage mode"));
      });
  const auto loaded = controller.Load();
  if (!loaded) {
    std::cerr << "error: " << loaded.error().message() << "\n";
    return 1;
  }

  if (options.dump_history) {
    std::cout << "# history (" << controller.history().size() << " entries)\n";
    for (const auto& entry : controller.history().All()) {
      std::cout << entry.last_visit << "\t" << entry.visit_count << "\t"
                << (entry.title.empty() ? "-" : entry.title) << "\t" << entry.url << "\n";
    }
  }
  if (options.dump_bookmarks) {
    std::cout << "# bookmarks (" << controller.bookmarks().size() << ")\n";
    for (const auto& b : controller.bookmarks().All()) {
      std::cout << (b.folder.empty() ? "-" : b.folder) << "\t" << (b.title.empty() ? "-" : b.title)
                << "\t" << b.url << "\n";
    }
  }
  if (options.show_cookies) {
    std::cout << "# cookies (" << controller.cookies().size() << ")\n";
    for (const auto& c : controller.cookies().All()) {
      std::cout << (c.host_only ? c.domain : "." + c.domain) << "\t" << c.path << "\t" << c.name
                << "\t" << c.value << "\t" << (c.secure ? "Secure " : "")
                << (c.http_only ? "HttpOnly " : "")
                << (c.same_site.empty() ? "" : "SameSite=" + c.same_site) << "\n";
    }
  }
  return 0;
}

// Prints a JavaScript completion value (objects as JSON) to stdout.
void PrintJsResult(const neko::javascript::ScriptValue& value)
{
  if (value.Kind() == neko::javascript::ValueKind::kObject) {
    auto json = value.JsonStringify();
    if (json.has_value()) {
      std::cout << json.value() << "\n";
      return;
    }
  }
  auto str = value.ToString();
  if (str.has_value())
    std::cout << str.value() << "\n";
}

} // namespace

int main(int argc, char** argv)
{
  const neko::browser::ParseResult parsed = neko::browser::ParseCommandLine(argc, argv);

  switch (parsed.action) {
  case neko::browser::ParseResult::Action::kHelp:
    std::cout << neko::browser::UsageText();
    return 0;
  case neko::browser::ParseResult::Action::kVersion:
    std::cout << neko::base::GetProjectName() << ' ' << neko::base::GetVersionString() << '\n';
    return 0;
  case neko::browser::ParseResult::Action::kError:
    std::cerr << "error: " << parsed.error_message << "\n\n" << neko::browser::UsageText();
    return 2;
  case neko::browser::ParseResult::Action::kRun:
    break;
  }

  neko::base::Logger::Instance().SetLevel(parsed.options.log_level);
  NEKO_LOG_INFO("neko-browser " + std::string(neko::base::GetVersionString()));

  const std::string profile_dir = parsed.options.profile_name.has_value()
                                      ? parsed.options.profile_name.value()
                                      : DefaultProfileDir();

  // -------------------------------------------------------------------------
  // Storage commands (history / bookmarks / cookies).
  // -------------------------------------------------------------------------
  if (parsed.options.dump_history || parsed.options.dump_bookmarks || parsed.options.show_cookies) {
    return RunStorageCommands(parsed.options, profile_dir);
  }

  // -------------------------------------------------------------------------
  // Content tools (pdf / audio / image / download).
  // -------------------------------------------------------------------------
  if (parsed.options.extract_pdf_path.has_value()) {
    auto bytes = neko::storage::ReadFile(parsed.options.extract_pdf_path.value());
    if (!bytes) {
      std::cerr << "error: " << bytes.error().message() << "\n";
      return 1;
    }
    auto doc = neko::pdf::ExtractText(bytes.value());
    if (!doc) {
      std::cerr << "error: " << doc.error().message() << "\n";
      return 1;
    }
    std::cout << "Title: " << (doc.value().title.empty() ? "(none)" : doc.value().title)
              << "\nPages: " << doc.value().page_count << "\n";
    for (const auto& page : doc.value().pages) {
      std::cout << "\n--- Page " << (page.index + 1) << " ---\n" << page.text << "\n";
    }
    return 0;
  }

  if (parsed.options.audio_info_path.has_value()) {
    auto bytes = neko::storage::ReadFile(parsed.options.audio_info_path.value());
    if (!bytes) {
      std::cerr << "error: " << bytes.error().message() << "\n";
      return 1;
    }
    auto audio = neko::media::DecodeWav(bytes.value());
    if (!audio) {
      std::cerr << "error: " << audio.error().message() << "\n";
      return 1;
    }
    std::cout << "sample rate : " << audio.value().sample_rate << " Hz\n"
              << "channels    : " << audio.value().channels << "\n"
              << "bit depth   : " << audio.value().bits_per_sample << "\n"
              << "samples     : " << audio.value().samples.size() << "\n"
              << "duration    : " << audio.value().duration_seconds() << " s\n";
    return 0;
  }

  if (parsed.options.image_info_path.has_value()) {
    auto bytes = neko::storage::ReadFile(parsed.options.image_info_path.value());
    if (!bytes) {
      std::cerr << "error: " << bytes.error().message() << "\n";
      return 1;
    }
    auto image = neko::image::DecodeImage(bytes.value());
    if (!image) {
      std::cerr << "error: " << image.error().message() << "\n";
      return 1;
    }
    std::cout << "format      : " << (neko::image::IsPng(bytes.value()) ? "PNG" : "JPEG") << "\n"
              << "size        : " << image.value().width << " x " << image.value().height << "\n";
    if (parsed.options.image_out_ppm.has_value()) {
      const auto written = WriteImagePpm(parsed.options.image_out_ppm.value(), image.value());
      if (!written) {
        std::cerr << "error: " << written.error().message() << "\n";
        return 1;
      }
      std::cout << "wrote: " << parsed.options.image_out_ppm.value() << "\n";
    }
    return 0;
  }

  if (parsed.options.download_url.has_value()) {
    neko::browser::BrowserController controller(profile_dir);
    const auto loaded = controller.Load();
    if (!loaded) {
      std::cerr << "error: " << loaded.error().message() << "\n";
      return 1;
    }
    auto url = neko::url::Url::Parse(parsed.options.download_url.value());
    if (!url) {
      std::cerr << "error: " << url.error().message() << "\n";
      return 1;
    }
    const std::string dir = parsed.options.download_dir.has_value()
                                ? parsed.options.download_dir.value()
                                : profile_dir + "/downloads";
    neko::browser::DownloadManager manager(dir);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto result =
        manager.Start(url.value(), controller.cookies().CookieHeaderFor(url.value(), now));
    if (!result) {
      std::cerr << "error: " << result.error().message() << "\n";
      return 1;
    }
    std::cout << "downloaded: " << result.value().filename << " (" << result.value().received_bytes
              << " bytes)\n";
    return 0;
  }

  // -------------------------------------------------------------------------
  // JavaScript (--eval <script>).
  // -------------------------------------------------------------------------
  if (parsed.options.eval_script.has_value()) {
    neko::javascript::ScriptEngine engine;
    engine.SetConsoleSink([](std::string_view level, std::string_view text) {
      std::cout << "[" << level << "] " << text << "\n";
    });
    const auto result = engine.Evaluate(parsed.options.eval_script.value());
    if (!result.has_value()) {
      std::cerr << "error: " << result.error().message() << "\n";
      return 1;
    }
    if (result.value().Kind() != neko::javascript::ValueKind::kUndefined) {
      PrintJsResult(result.value());
    }
    return 0;
  }

  // -------------------------------------------------------------------------
  // Page commands (--url / --dump-dom / --screenshot).
  // -------------------------------------------------------------------------
  neko::renderer::Page page;

  if (parsed.options.url.has_value()) {
    const neko::base::Result<void> loaded = LoadTarget(page, parsed.options.url.value());
    if (!loaded) {
      std::cerr << "error: " << loaded.error().message() << "\n";
      return 1;
    }
    NEKO_LOG_INFO("loaded document title: " + page.document()->Title());
  } else {
    std::cout << "no URL given; run with --url <url> to load a page.\n";
  }

  if (parsed.options.dump_dom) {
    const std::string dump = page.DumpDom();
    std::cout << dump;
    if (!dump.empty() && dump.back() != '\n') {
      std::cout << '\n';
    }
  }

  if (parsed.options.screenshot_path.has_value()) {
    constexpr float kViewportWidth = 800;
    constexpr int kMinHeight = 600;
    page.Layout(kViewportWidth);
    const float content_height =
        page.layout_root() != nullptr ? page.layout_root()->height : kMinHeight;
    const int height = std::max(kMinHeight, static_cast<int>(content_height) + 40);
    neko::paint::Rasterizer image = page.Rasterize(800, height);
    const auto written = neko::paint::WritePpm(parsed.options.screenshot_path.value(), image);
    if (!written) {
      std::cerr << "error: " << written.error().message() << "\n";
      return 1;
    }
    std::cout << "wrote screenshot: " << parsed.options.screenshot_path.value() << "\n";
  }

  NEKO_LOG_INFO("done");
  return 0;
}
