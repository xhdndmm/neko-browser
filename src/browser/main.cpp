#include <algorithm>
#include <iostream>
#include <string>

#include "neko/base/logging.h"
#include "neko/base/status.h"
#include "neko/base/version.h"
#include "neko/browser/browser_options.h"
#include "neko/network/http.h"
#include "neko/paint/rasterizer.h"
#include "neko/renderer/page.h"
#include "neko/url/url.h"

namespace {

// Loads a URL (http/https via the network stack) or a local file into the page.
neko::base::Result<void> LoadTarget(neko::renderer::Page& page, const std::string& target) {
  const auto parsed = neko::url::Url::Parse(target);
  if (parsed.has_value()) {
    const neko::url::Url& url = parsed.value();
    if (url.scheme() == "http") {
      NEKO_LOG_INFO("fetching " + url.Serialize());
      const auto response = neko::network::HttpGet(url);
      if (!response) {
        return neko::base::Err(response.error());
      }
      NEKO_LOG_INFO("HTTP " + std::to_string(response.value().status_code) + " (" +
                    std::to_string(response.value().body.size()) + " bytes)");
      return page.LoadHtml(response.value().body);
    }
    if (url.scheme() == "https") {
      return neko::base::Err(neko::base::Error::NotImplemented(
          "https:// requires TLS (planned for a later phase)"));
    }
    if (url.scheme() == "file") {
      return page.LoadFile(url.path());
    }
    return neko::base::Err(
        neko::base::Error::NotImplemented("unsupported URL scheme: " + url.scheme()));
  }
  // No recognizable scheme: treat the argument as a local file path.
  return page.LoadFile(target);
}

}  // namespace

int main(int argc, char** argv) {
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
  NEKO_LOG_INFO("neko-browser " + std::string(neko::base::GetVersionString()) + " (Phase 6)");

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
