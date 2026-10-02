// neko-browser CLI (headless).
//
//  - Page commands:  --url <url|path> [--dump-dom] [--screenshot <path>]
//  - Storage:        --dump-history | --dump-bookmarks | --show-cookies
//  - Content tools:  --download <url> | --extract-pdf <file> |
//                    --audio-info <file> | --image-info <file> [--image-out <path>]
//  - GUI:            run the separate neko_browser_gui executable.

#include "neko/base/logging.h"
#include "neko/base/status.h"
#include "neko/base/thread_pool.h"
#include "neko/base/version.h"
#include "neko/browser/browser_controller.h"
#include "neko/browser/browser_options.h"
#include "neko/browser/hyperlink.h"
#include "neko/browser/network_host.h"
#include "neko/browser/network_session.h"
#include "neko/browser/page_scripts.h"
#include "neko/browser/renderer_host.h"
#include "neko/browser/renderer_protocol.h"
#include "neko/browser/renderer_session_host.h"
#include "neko/image/image.h"
#include "neko/ipc/channel.h"
#include "neko/javascript/script_engine.h"
#include "neko/media/audio.h"
#include "neko/media/video.h"
#include "neko/network/http.h"
#include "neko/paint/ppm_writer.h"
#include "neko/paint/rasterizer.h"
#include "neko/pdf/pdf.h"
#include "neko/renderer/page.h"
#include "neko/security/origin.h"
#include "neko/storage/file_util.h"
#include "neko/storage/indexed_db.h"
#include "neko/storage/local_storage.h"
#include "neko/url/url.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <csignal>
#endif

namespace {

// Fetches http(s) URLs over the network and file:// URLs from disk, so local
// pages resolve their relative subresources the same way a served page does.
neko::base::Result<neko::network::HttpResponse> FetchAny(const neko::url::Url& url,
                                                         std::string_view)
{
  if (url.scheme() == "file") {
    auto bytes = neko::storage::ReadFile(url.path());
    if (!bytes) {
      return neko::base::Err(bytes.error());
    }
    neko::network::HttpResponse response;
    response.status_code = 200;
    response.body = std::move(bytes.value());
    return response;
  }
  return neko::network::HttpGet(url);
}

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

// Dispatches the image "load"/"error" events a subresource pass queued on the
// page and returns how many were delivered.  The CLI drives the binder
// directly, so it drains the queue here instead of through
// BrowserController::FirePendingImageEvents.  Elements replaced since the
// fetch (navigation, innerHTML) are skipped; draining without a binder still
// resets the queue.
std::size_t FirePendingImageEvents(neko::renderer::Page& page,
                                   neko::javascript::DomBinder* script_runtime)
{
  const std::vector<std::pair<const neko::dom::Element*, bool>> events =
      page.TakePendingImageEvents();
  if (script_runtime == nullptr) {
    return 0; // no scripts: nothing observes the events
  }
  std::size_t fired = 0;
  for (const auto& [element, loaded] : events) {
    if (!page.ContainsElement(element)) {
      continue;
    }
    script_runtime->DispatchNonBubblingEvent(*const_cast<neko::dom::Element*>(element),
                                             loaded ? "load" : "error");
    ++fired;
  }
  return fired;
}

// |script_runtime|, when non-null, receives the page's JS binder so the caller
// can drive the event loop after the load (see PumpUntilQuiet in main()).
// |document_fetch|, when set, replaces the in-process network stack for the
// top-level document fetch (network-process mode, ADR 0016 M3a); it is
// forwarded to recursive script navigations.
// |final_url|, when non-null, receives the URL the document ended up at
// (post-redirect), which the caller uses as the base for a late subresource
// pass after the script/timer pump (lazy-loaded images).
neko::base::Result<void> LoadTarget(
    neko::renderer::Page& page,
    const std::string& target,
    neko::storage::LocalStorage* local_storage,
    neko::storage::IndexedDbStore* indexed_db,
    int depth = 0,
    std::shared_ptr<neko::javascript::DomBinder>* script_runtime = nullptr,
    const std::function<neko::base::Result<neko::network::HttpResponse>(const neko::url::Url&)>&
        document_fetch = {},
    std::string* final_url = nullptr);

// Loads a bare local path (no scheme) into the page and fetches its
// subresources against an absolute file:// base so relative URLs resolve.
neko::base::Result<void> LoadLocalTarget(
    neko::renderer::Page& page,
    const std::string& target,
    neko::storage::LocalStorage* local_storage,
    neko::storage::IndexedDbStore* indexed_db,
    int depth,
    std::shared_ptr<neko::javascript::DomBinder>* script_runtime,
    const std::function<neko::base::Result<neko::network::HttpResponse>(const neko::url::Url&)>&
        document_fetch,
    std::string* final_url = nullptr)
{
  const auto r = page.LoadFile(target);
  if (!r) {
    return r;
  }
  neko::browser::ScriptRequestedNavigation requested;
  neko::browser::PageScriptServices services;
  services.local_storage = local_storage;
  services.indexed_db = indexed_db;
  services.origin = "null";
  if (script_runtime != nullptr) {
    *script_runtime = neko::browser::RunPageScripts(
        page,
        "",
        [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
        [](std::string_view level, std::string_view text) {
          std::cout << "[" << level << "] " << text << "\n";
        },
        services,
        &requested);
  } else {
    neko::browser::RunPageScripts(
        page,
        "",
        [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
        [](std::string_view level, std::string_view text) {
          std::cout << "[" << level << "] " << text << "\n";
        },
        services,
        &requested);
  }
  if (!requested.url.empty()) {
    NEKO_LOG_INFO("script navigated to " + requested.url);
    return LoadTarget(page,
                      requested.url,
                      local_storage,
                      indexed_db,
                      depth + 1,
                      script_runtime,
                      document_fetch,
                      final_url);
  }
  // Local page (opened by path without a scheme): fetch its subresources
  // against an absolute file:// base so relative URLs resolve.
  neko::base::ThreadPool pool;
  const std::string base_url =
      "file://" + std::filesystem::absolute(std::filesystem::path(target)).generic_string();
  neko::browser::FetchExternalStylesheets(page, base_url, FetchAny, pool);
  neko::browser::FetchPageImages(page, base_url, FetchAny, pool);
  neko::browser::FetchPageVideos(page, base_url, FetchAny, pool);
  FirePendingImageEvents(page, script_runtime != nullptr ? script_runtime->get() : nullptr);
  if (final_url != nullptr) {
    *final_url = base_url;
  }
  return neko::base::Ok();
}

// Loads a URL (http via the network stack) or a local file into the page.
// Follows page-script navigation requests (window.location) recursively, up to
// a depth cap so a redirect loop terminates.  When |local_storage| and
// |indexed_db| are provided, page scripts get localStorage/indexedDB scoped
// to the loaded page's origin.
neko::base::Result<void> LoadTarget(
    neko::renderer::Page& page,
    const std::string& target,
    neko::storage::LocalStorage* local_storage,
    neko::storage::IndexedDbStore* indexed_db,
    int depth,
    std::shared_ptr<neko::javascript::DomBinder>* script_runtime,
    const std::function<neko::base::Result<neko::network::HttpResponse>(const neko::url::Url&)>&
        document_fetch,
    std::string* final_url)
{
  constexpr int kMaxNavigationDepth = 20;
  if (depth >= kMaxNavigationDepth) {
    NEKO_LOG_WARNING("navigation chain too deep; stopped at " + target);
    return neko::base::Ok();
  }
#ifdef _WIN32
  // Windows drive and UNC paths are also syntactically valid URLs ("D:/dir/
  // page.html" parses as the one-letter scheme "d"): they are filesystem
  // references and must reach the local-file loader before the URL parse
  // (mirrors BrowserController::NavigateToUrl, so the renderer child accepts
  // the same path forms the browser process does).
  if (neko::browser::IsWindowsLocalPath(target)) {
    return LoadLocalTarget(
        page, target, local_storage, indexed_db, depth, script_runtime, document_fetch, final_url);
  }
#endif
  const auto parsed = neko::url::Url::Parse(target);
  if (parsed.has_value()) {
    const neko::url::Url& url = parsed.value();
    if (url.scheme() == "http" || url.scheme() == "https") {
      NEKO_LOG_INFO("fetching " + url.Serialize());
      // Network-process mode routes the document fetch through the child
      // (ADR 0016 M3a); everything else uses the in-process stack.
      const auto response = document_fetch ? document_fetch(url) : neko::network::HttpGet(url);
      if (!response) {
        return neko::base::Err(response.error());
      }
      NEKO_LOG_INFO("HTTP " + std::to_string(response.value().status_code) + " (" +
                    std::to_string(response.value().body.size()) + " bytes)");
      // Transcode the body per the HTTP charset and in-document declarations.
      const std::optional<neko::base::encoding::Charset> http_charset =
          neko::base::encoding::CharsetFromHttpHeader(response.value().GetHeader("content-type"));
      const auto r = page.LoadHtml(response.value().body,
                                   http_charset.value_or(neko::base::encoding::Charset::kUnknown));
      if (!r) {
        return r;
      }
      neko::base::ThreadPool pool;
      // Fetch and apply external <link rel=stylesheet> sheets before scripts.
      neko::browser::FetchExternalStylesheets(
          page,
          url.Serialize(),
          [](const neko::url::Url& u, std::string_view) { return neko::network::HttpGet(u); },
          pool);
      // Fetch and register @font-face web fonts so layout/paint see them.
      neko::browser::FetchWebFonts(page, url.Serialize(), FetchAny, pool);
      // Phase 8 M2: execute the page's scripts (inline + external src=,
      // async/defer); scripts may mutate the DOM and RunPageScripts
      // re-applies styles inside.
      neko::browser::ScriptRequestedNavigation requested;
      neko::browser::PageScriptServices services;
      services.local_storage = local_storage;
      services.indexed_db = indexed_db;
      services.origin = neko::security::Origin::FromUrl(url).Serialize();
      if (script_runtime != nullptr) {
        *script_runtime = neko::browser::RunPageScripts(
            page,
            url.Serialize(),
            [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
            [](std::string_view level, std::string_view text) {
              std::cout << "[" << level << "] " << text << "\n";
            },
            services,
            &requested);
      } else {
        neko::browser::RunPageScripts(
            page,
            url.Serialize(),
            [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
            [](std::string_view level, std::string_view text) {
              std::cout << "[" << level << "] " << text << "\n";
            },
            services,
            &requested);
      }
      // A script may have redirected the page (e.g. location.replace()).
      if (!requested.url.empty()) {
        NEKO_LOG_INFO("script navigated to " + requested.url);
        return LoadTarget(page,
                          requested.url,
                          local_storage,
                          indexed_db,
                          depth + 1,
                          script_runtime,
                          document_fetch,
                          final_url);
      }
      if (requested.is_reload) {
        NEKO_LOG_INFO("script reloaded " + url.Serialize());
        return LoadTarget(page,
                          url.Serialize(),
                          local_storage,
                          indexed_db,
                          depth + 1,
                          script_runtime,
                          document_fetch);
      }
      // Scripts may have injected <link rel=stylesheet> (e.g. Bing's
      // as-css-link) after the initial stylesheet pass; fetch those so the
      // fully styled cascade (wallpaper/theme rules) applies.
      neko::browser::FetchExternalStylesheets(
          page,
          url.Serialize(),
          [](const neko::url::Url& u, std::string_view) { return neko::network::HttpGet(u); },
          pool);
      // Fonts declared by script-injected sheets load here too.
      neko::browser::FetchWebFonts(page, url.Serialize(), FetchAny, pool);
      // Fetch and decode the page's <img>/<video> subresources (headless path).
      neko::browser::FetchPageImages(
          page,
          url.Serialize(),
          [](const neko::url::Url& u, std::string_view) { return neko::network::HttpGet(u); },
          pool);
      neko::browser::FetchPageVideos(
          page,
          url.Serialize(),
          [](const neko::url::Url& u, std::string_view) { return neko::network::HttpGet(u); },
          pool);
      // Deliver the queued image load/error events: page onload="..."
      // attributes and listeners flip placeholder/fallback state off them.
      FirePendingImageEvents(page, script_runtime != nullptr ? script_runtime->get() : nullptr);
      if (final_url != nullptr) {
        *final_url = url.Serialize();
      }
      return neko::base::Ok();
    }
    if (url.scheme() == "file") {
      const auto r = page.LoadFile(url.path());
      if (!r) {
        return r;
      }
      neko::browser::ScriptRequestedNavigation requested;
      neko::browser::PageScriptServices services;
      services.local_storage = local_storage;
      services.indexed_db = indexed_db;
      // Local pages share the opaque file origin (same treatment as the
      // controller's non-URL content).
      services.origin = "null";
      if (script_runtime != nullptr) {
        *script_runtime = neko::browser::RunPageScripts(
            page,
            "",
            [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
            [](std::string_view level, std::string_view text) {
              std::cout << "[" << level << "] " << text << "\n";
            },
            services,
            &requested);
      } else {
        neko::browser::RunPageScripts(
            page,
            "",
            [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
            [](std::string_view level, std::string_view text) {
              std::cout << "[" << level << "] " << text << "\n";
            },
            services,
            &requested);
      }
      if (!requested.url.empty()) {
        NEKO_LOG_INFO("script navigated to " + requested.url);
        return LoadTarget(page,
                          requested.url,
                          local_storage,
                          indexed_db,
                          depth + 1,
                          script_runtime,
                          document_fetch,
                          final_url);
      }
      // Fetch the page's subresources: relative URLs resolve against the
      // file:// base so local pages behave like served ones.
      neko::base::ThreadPool pool;
      neko::browser::FetchExternalStylesheets(page, url.Serialize(), FetchAny, pool);
      neko::browser::FetchWebFonts(page, url.Serialize(), FetchAny, pool);
      neko::browser::FetchPageImages(page, url.Serialize(), FetchAny, pool);
      neko::browser::FetchPageVideos(page, url.Serialize(), FetchAny, pool);
      FirePendingImageEvents(page, script_runtime != nullptr ? script_runtime->get() : nullptr);
      if (final_url != nullptr) {
        *final_url = url.Serialize();
      }
      return neko::base::Ok();
    }
    return neko::base::Err(
        neko::base::Error::NotImplemented("unsupported URL scheme: " + url.scheme()));
  }
  // Not a URL at all: a bare local path.
  return LoadLocalTarget(
      page, target, local_storage, indexed_db, depth, script_runtime, document_fetch, final_url);
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

// Both renderer child modes own stdout for the wire protocol: the parent reads
// it as a byte stream of frames.  Anything the engine prints to std::cout (the
// page-script console printer, CLI-style diagnostics) therefore has to go to
// stderr instead — a single stray line would be parsed as the next frame
// header and fail as "frame length exceeds the cap".
void RedirectStdoutToStderr()
{
  std::cout.rdbuf(std::cerr.rdbuf());
}

} // namespace

// Renderer child mode (ADR 0016 M1): the browser process spawns this binary
// with --renderer-child and speaks the renderer protocol over stdin/stdout.
// One LoadRequest per process for now (no session reuse); the child loads
// the page through the same in-process pipeline the CLI uses, rasterizes the
// viewport and replies with the frame + DOM text.
int RunRendererChild()
{
  neko::ipc::Channel channel = neko::ipc::Channel::FromStdio();

  const auto request_frame = channel.Receive();
  if (!request_frame.has_value()) {
    std::cerr << "renderer child: failed to read request: " << request_frame.error().message()
              << "\n";
    return 1;
  }
  const auto request = neko::browser::DecodeLoadRequest(request_frame.value());
  if (!request.has_value()) {
    std::cerr << "renderer child: malformed request: " << request.error().message() << "\n";
    return 1;
  }

  neko::browser::RendererLoadResult result;
  neko::renderer::Page page;
  const neko::base::Result<void> loaded =
      LoadTarget(page, request.value().url, /*local_storage=*/nullptr, /*indexed_db=*/nullptr);
  if (!loaded.has_value()) {
    result.ok = false;
    result.error = loaded.error().message();
  } else {
    const int width = std::max(1, request.value().viewport_width);
    const int height = std::max(1, request.value().viewport_height);
    // A fixed initial viewport height gives percentage-height chains a
    // definite basis, mirroring the CLI screenshot path.
    page.Layout(static_cast<float>(width), static_cast<float>(height));
    const float content_height =
        page.layout_root() != nullptr ? page.layout_root()->height : static_cast<float>(height);
    const int full_height = std::max(height, static_cast<int>(content_height) + 40);
    // Rasterize in parallel bands: the child is CPU-bound on paint once the
    // page is loaded, and a full-height buffer gives the band split plenty of
    // rows to work with.
    neko::base::ThreadPool pool;
    neko::paint::Rasterizer raster = page.Rasterize(width, full_height, /*y_offset=*/0, &pool);
    result.ok = true;
    result.width = raster.width();
    result.height = raster.height();
    result.rgba = raster.pixels();
    result.dom = page.DumpDom();
    result.title = page.document()->Title();
  }

  const auto encoded = neko::browser::EncodeLoadResult(result);
  if (!encoded.has_value() || !channel.Send(encoded.value())) {
    std::cerr << "renderer child: failed to send the result\n";
    return 1;
  }
  return 0;
}

int main(int argc, char** argv)
{
#ifndef _WIN32
  // A peer (e.g. a renderer child) closing its pipe mid-write must surface
  // as a failed write, not SIGPIPE terminating the browser process.
  std::signal(SIGPIPE, SIG_IGN);
#endif
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

  // stdout belongs to the wire protocol in both child modes; move any
  // std::cout writer (the page-script console printer) to stderr before the
  // child serves its first request.
  if (parsed.options.renderer_child || parsed.options.renderer_session ||
      parsed.options.network_child) {
    RedirectStdoutToStderr();
  }

  // Renderer child mode: serve one load request on stdin/stdout and exit
  // (spawned by browser::RendererHost; ADR 0016 M1).
  if (parsed.options.renderer_child) {
    return RunRendererChild();
  }

  // Renderer session mode: serve interaction ops for a live page until the
  // browser closes the pipe (spawned by RendererSession; ADR 0016 M2).
  if (parsed.options.renderer_session) {
    return neko::browser::RunRendererSession();
  }

  // Network child mode: serve network requests on stdin/stdout until the
  // browser closes the pipe (spawned by NetworkSession; ADR 0016 M3a).
  if (parsed.options.network_child) {
    return neko::browser::RunNetworkChild();
  }

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
    if (parsed.options.pdf_render_out.has_value()) {
      const auto rendered =
          neko::pdf::RenderPage(bytes.value(), parsed.options.pdf_page, parsed.options.pdf_scale);
      if (!rendered) {
        std::cerr << "error: pdf render: " << rendered.error().message() << "\n";
        return 1;
      }
      const auto written = WriteImagePpm(parsed.options.pdf_render_out.value(), rendered.value());
      if (!written) {
        std::cerr << "error: " << written.error().message() << "\n";
        return 1;
      }
      std::cout << "wrote: " << parsed.options.pdf_render_out.value() << "\n";
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
    const char* format = "unknown";
    if (neko::image::IsPng(bytes.value())) {
      format = "PNG";
    } else if (neko::image::IsJpeg(bytes.value())) {
      format = "JPEG";
    } else if (neko::image::IsGif(bytes.value())) {
      format = "GIF";
    } else if (neko::image::IsWebp(bytes.value())) {
      format = "WebP";
    } else if (neko::image::IsAvif(bytes.value())) {
      format = "AVIF";
    }
    std::cout << "format      : " << format << "\n"
              << "size        : " << image.value().width << " x " << image.value().height << "\n";
    if (neko::image::IsGif(bytes.value())) {
      const auto anim = neko::image::DecodeGifAnimation(bytes.value());
      if (anim.has_value()) {
        std::cout << "frames      : " << anim.value().frames.size()
                  << (anim.value().loop_count == 0 ? " (loop forever)" : "") << "\n";
      }
    }
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

  if (parsed.options.video_info_path.has_value()) {
    auto bytes = neko::storage::ReadFile(parsed.options.video_info_path.value());
    if (!bytes) {
      std::cerr << "error: " << bytes.error().message() << "\n";
      return 1;
    }
    auto video = neko::media::DecodeVideo(bytes.value());
    if (!video) {
      std::cerr << "error: " << video.error().message() << "\n";
      return 1;
    }
    std::cout << "container   : " << video.value().format_name << "\n"
              << "codec       : " << video.value().codec_name << "\n"
              << "size        : " << video.value().width << " x " << video.value().height << "\n"
              << "duration    : " << video.value().duration_seconds << " s\n"
              << "frame rate  : " << video.value().frame_rate << " fps\n"
              << "frames      : " << video.value().frames.size() << "\n";
    if (parsed.options.video_out_ppm.has_value()) {
      if (video.value().frames.empty()) {
        std::cerr << "error: no frames decoded\n";
        return 1;
      }
      const auto written =
          WriteImagePpm(parsed.options.video_out_ppm.value(), video.value().frames[0].image);
      if (!written) {
        std::cerr << "error: " << written.error().message() << "\n";
        return 1;
      }
      std::cout << "wrote: " << parsed.options.video_out_ppm.value() << "\n";
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
  // Page scripts get localStorage/indexedDB scoped to the loaded origin; the
  // stores persist into the profile and outlive the page load.
  neko::storage::LocalStorage local_storage(profile_dir);
  neko::storage::IndexedDbStore indexed_db(profile_dir);
  if (!local_storage.Load()) {
    NEKO_LOG_WARNING("failed to load local storage profile");
  }
  if (!indexed_db.Load()) {
    NEKO_LOG_WARNING("failed to load indexedDB profile");
  }
  neko::renderer::Page page;
  // The renderer-process result (ADR 0016 M1): set when --renderer-process
  // routes the load through a child process instead of the in-process
  // pipeline.
  std::optional<neko::browser::RendererLoadResult> renderer_result;

  if (parsed.options.renderer_process) {
    if (!parsed.options.url.has_value()) {
      std::cerr << "error: --renderer-process requires --url\n";
      return 1;
    }
    if (parsed.options.network_process) {
      NEKO_LOG_WARNING("--network-process does not apply to --renderer-process yet (the "
                       "renderer child fetches with its own stack); ignoring it");
    }
    neko::browser::RendererHost host(neko::browser::SelfExecutablePath());
    auto loaded = host.Load(parsed.options.url.value(), /*width=*/800, /*height=*/600);
    if (!loaded.has_value()) {
      std::cerr << "error: " << loaded.error().message() << "\n";
      return 1;
    }
    renderer_result = std::move(loaded.value());
    NEKO_LOG_INFO("renderer child loaded document title: " + renderer_result->title);
  } else if (parsed.options.url.has_value()) {
    // Network-process mode (ADR 0016 M3a): the top-level document fetch runs
    // in the child.  Subresource fetches still use the in-process stack in
    // this milestone (documented limitation).
    std::shared_ptr<neko::browser::NetworkSession> network_session;
    std::function<neko::base::Result<neko::network::HttpResponse>(const neko::url::Url&)>
        document_fetch;
    if (parsed.options.network_process) {
      auto spawned = neko::browser::NetworkSession::Spawn(neko::browser::SelfExecutablePath());
      if (!spawned.has_value()) {
        std::cerr << "error: cannot start network process: " << spawned.error().message() << "\n";
        return 1;
      }
      network_session = spawned.value();
      document_fetch = [network_session](const neko::url::Url& fetch_url) {
        return network_session->Fetch(fetch_url,
                                      /*cookie_header=*/{},
                                      neko::browser::NetworkSession::CookieLookupFn{});
      };
    }
    std::shared_ptr<neko::javascript::DomBinder> script_runtime;
    std::string loaded_base_url;
    const neko::base::Result<void> loaded = LoadTarget(page,
                                                       parsed.options.url.value(),
                                                       &local_storage,
                                                       &indexed_db,
                                                       /*depth=*/0,
                                                       &script_runtime,
                                                       document_fetch,
                                                       &loaded_base_url);
    if (!loaded) {
      std::cerr << "error: " << loaded.error().message() << "\n";
      return 1;
    }
    NEKO_LOG_INFO("loaded document title: " + page.document()->Title());
    // Drive the page's event loop before anything observes the document.  The
    // GUI pumps this from a 50 ms QTimer, but the headless entry points had no
    // event loop at all, so --screenshot / --dump-dom used to capture the page
    // in its "synchronous script only" state: no setTimeout, setInterval or
    // requestAnimationFrame callback had run, and that is where most of a real
    // page's content lives (framework hydration, lazy chunks, deferred
    // rendering).  Bounded by both an iteration cap and a 250 ms quiet budget
    // so a page that schedules work forever still terminates.
    if (script_runtime != nullptr) {
      neko::browser::PumpScriptTimersUntilQuiet(*script_runtime);
      if (script_runtime->TakeDomDirty()) {
        // Timers mutated the DOM; re-run the cascade so the measured layout
        // reflects it.
        page.ReapplyStyles();
      }
    }
    // Lazy-loaded images: pages assign img.src / background-image from timer
    // and event callbacks *after* the initial subresource pass inside
    // LoadTarget, so those sources were never fetched and the screenshot
    // showed placeholders (bilibili's feed).  ClaimPendingImageSources skips
    // everything the initial pass claimed, so this fetches only what the
    // callbacks added.  Each round also delivers the queued load/error events:
    // a handler may assign a fallback source (one extra round picks it up),
    // and a round that fires nothing means the page settled.  Claimed sources
    // keep the steady state free of re-downloads.
    if (!loaded_base_url.empty()) {
      neko::base::ThreadPool late_pool;
      neko::javascript::DomBinder* binder =
          script_runtime != nullptr ? script_runtime.get() : nullptr;
      constexpr int kMaxRounds = 3;
      for (int round = 0; round < kMaxRounds; ++round) {
        neko::browser::FetchPageImages(page, loaded_base_url, FetchAny, late_pool);
        if (FirePendingImageEvents(page, binder) == 0) {
          break;
        }
      }
      if (binder != nullptr && binder->TakeDomDirty()) {
        // onload/onerror handlers may have toggled classes or inline styles.
        page.ReapplyStyles();
      }
    }
  } else {
    std::cout << "no URL given; run with --url <url> to load a page.\n";
  }

  if (parsed.options.dump_dom) {
    const std::string dump = renderer_result.has_value() ? renderer_result->dom : page.DumpDom();
    std::cout << dump;
    if (!dump.empty() && dump.back() != '\n') {
      std::cout << '\n';
    }
  }

  if (parsed.options.screenshot_path.has_value()) {
    if (renderer_result.has_value()) {
      // The child already rasterized the page; composite alpha over white
      // and write the frame.
      neko::image::Image frame;
      frame.width = renderer_result->width;
      frame.height = renderer_result->height;
      frame.rgba = renderer_result->rgba;
      const auto written = WriteImagePpm(parsed.options.screenshot_path.value(), frame);
      if (!written) {
        std::cerr << "error: " << written.error().message() << "\n";
        return 1;
      }
      std::cout << "wrote screenshot (renderer process): " << parsed.options.screenshot_path.value()
                << "\n";
    } else {
      constexpr float kViewportWidth = 800;
      constexpr int kMinHeight = 600;
      // A fixed initial viewport height gives percentage-height chains
      // (html/body/... { height: 100% }) a definite basis to resolve against,
      // matching a browser window rather than an unbounded "whole page" canvas.
      page.Layout(kViewportWidth, kMinHeight);
      const float content_height =
          page.layout_root() != nullptr ? page.layout_root()->height : kMinHeight;
      const int height = std::max(kMinHeight, static_cast<int>(content_height) + 40);
      // Rasterize band by band instead of allocating one full-page RGBA
      // buffer: a page hundreds of thousands of pixels tall (real pages can
      // be) would otherwise need gigabytes even though the screenshot itself
      // is written row by row.  PpmWriter streams the encoded rows out, so
      // peak memory stays within one band.  The output is byte identical to
      // a single full-page rasterization.  Each band re-walks the display
      // list, so the band height also sets the memory/CPU trade-off: 4096
      // rows cap the band buffer at 800 * 4096 * 4 = 13 MiB, and a page
      // 460k rows tall only pays for ~112 passes over the list.
      constexpr int kBandHeight = 4096;
      auto writer =
          neko::paint::PpmWriter::Create(parsed.options.screenshot_path.value(), 800, height);
      if (!writer) {
        std::cerr << "error: " << writer.error().message() << "\n";
        return 1;
      }
      neko::paint::Rasterizer band(800, std::min(kBandHeight, height));
      // One pool for the whole loop; each band is rasterized in parallel
      // horizontal sub-bands.  The output stays byte-identical to the serial
      // path (RasterizeParallel honours the band's visible region).
      neko::base::ThreadPool pool;
      for (int y0 = 0; y0 < height; y0 += kBandHeight) {
        const int rows = std::min(kBandHeight, height - y0);
        page.RasterizeInto(band, 0, rows, static_cast<float>(y0), &pool);
        const auto appended = writer.value().AppendRows(band.pixels().data(), rows);
        if (!appended) {
          std::cerr << "error: " << appended.error().message() << "\n";
          return 1;
        }
      }
      const auto written = writer.value().Finish();
      if (!written) {
        std::cerr << "error: " << written.error().message() << "\n";
        return 1;
      }
      std::cout << "wrote screenshot: " << parsed.options.screenshot_path.value() << "\n";
    }
  }

  NEKO_LOG_INFO("done");
  return 0;
}
