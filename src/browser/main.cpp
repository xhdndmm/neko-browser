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
#include "neko/browser/page_scripts.h"
#include "neko/browser/renderer_host.h"
#include "neko/browser/renderer_protocol.h"
#include "neko/image/image.h"
#include "neko/ipc/channel.h"
#include "neko/javascript/script_engine.h"
#include "neko/media/audio.h"
#include "neko/media/video.h"
#include "neko/network/http.h"
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

// Loads a URL (http via the network stack) or a local file into the page.
// Follows page-script navigation requests (window.location) recursively, up to
// a depth cap so a redirect loop terminates.  When |local_storage| and
// |indexed_db| are provided, page scripts get localStorage/indexedDB scoped
// to the loaded page's origin.
neko::base::Result<void> LoadTarget(neko::renderer::Page& page,
                                    const std::string& target,
                                    neko::storage::LocalStorage* local_storage,
                                    neko::storage::IndexedDbStore* indexed_db,
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
      // Phase 8 M2: execute the page's scripts (inline + external src=,
      // async/defer); scripts may mutate the DOM and RunPageScripts
      // re-applies styles inside.
      neko::browser::ScriptRequestedNavigation requested;
      neko::browser::PageScriptServices services;
      services.local_storage = local_storage;
      services.indexed_db = indexed_db;
      services.origin = neko::security::Origin::FromUrl(url).Serialize();
      neko::browser::RunPageScripts(
          page,
          url.Serialize(),
          [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
          [](std::string_view level, std::string_view text) {
            std::cout << "[" << level << "] " << text << "\n";
          },
          services,
          &requested);
      // A script may have redirected the page (e.g. location.replace()).
      if (!requested.url.empty()) {
        NEKO_LOG_INFO("script navigated to " + requested.url);
        return LoadTarget(page, requested.url, local_storage, indexed_db, depth + 1);
      }
      if (requested.is_reload) {
        NEKO_LOG_INFO("script reloaded " + url.Serialize());
        return LoadTarget(page, url.Serialize(), local_storage, indexed_db, depth + 1);
      }
      // Scripts may have injected <link rel=stylesheet> (e.g. Bing's
      // as-css-link) after the initial stylesheet pass; fetch those so the
      // fully styled cascade (wallpaper/theme rules) applies.
      neko::browser::FetchExternalStylesheets(
          page,
          url.Serialize(),
          [](const neko::url::Url& u, std::string_view) { return neko::network::HttpGet(u); },
          pool);
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
      neko::browser::RunPageScripts(
          page,
          "",
          [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
          [](std::string_view level, std::string_view text) {
            std::cout << "[" << level << "] " << text << "\n";
          },
          services,
          &requested);
      if (!requested.url.empty()) {
        NEKO_LOG_INFO("script navigated to " + requested.url);
        return LoadTarget(page, requested.url, local_storage, indexed_db, depth + 1);
      }
      // Fetch the page's subresources: relative URLs resolve against the
      // file:// base so local pages behave like served ones.
      neko::base::ThreadPool pool;
      neko::browser::FetchExternalStylesheets(page, url.Serialize(), FetchAny, pool);
      neko::browser::FetchPageImages(page, url.Serialize(), FetchAny, pool);
      neko::browser::FetchPageVideos(page, url.Serialize(), FetchAny, pool);
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
  neko::browser::PageScriptServices services;
  services.local_storage = local_storage;
  services.indexed_db = indexed_db;
  services.origin = "null";
  neko::browser::RunPageScripts(
      page,
      "",
      [](const neko::url::Url& u) { return neko::network::HttpGet(u); },
      [](std::string_view level, std::string_view text) {
        std::cout << "[" << level << "] " << text << "\n";
      },
      services,
      &requested);
  if (!requested.url.empty()) {
    NEKO_LOG_INFO("script navigated to " + requested.url);
    return LoadTarget(page, requested.url, local_storage, indexed_db, depth + 1);
  }
  // Local page (opened by path without a scheme): fetch its subresources
  // against an absolute file:// base so relative URLs resolve.
  neko::base::ThreadPool pool;
  const std::string base_url =
      "file://" + std::filesystem::absolute(std::filesystem::path(target)).generic_string();
  neko::browser::FetchExternalStylesheets(page, base_url, FetchAny, pool);
  neko::browser::FetchPageImages(page, base_url, FetchAny, pool);
  neko::browser::FetchPageVideos(page, base_url, FetchAny, pool);
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

// Renderer child mode (ADR 0016 M1): the browser process spawns this binary
// with --renderer-child and speaks the renderer protocol over stdin/stdout.
// One LoadRequest per process for now (no session reuse); the child loads
// the page through the same in-process pipeline the CLI uses, rasterizes the
// viewport and replies with the frame + DOM text.
int RunRendererChild()
{
  neko::ipc::Channel channel = neko::ipc::Channel::FromHandles(0, 1);

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
    neko::paint::Rasterizer raster = page.Rasterize(width, full_height);
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

  // Renderer child mode: serve one load request on stdin/stdout and exit
  // (spawned by browser::RendererHost; ADR 0016 M1).
  if (parsed.options.renderer_child) {
    return RunRendererChild();
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
    neko::browser::RendererHost host(neko::browser::SelfExecutablePath());
    auto loaded = host.Load(parsed.options.url.value(), /*width=*/800, /*height=*/600);
    if (!loaded.has_value()) {
      std::cerr << "error: " << loaded.error().message() << "\n";
      return 1;
    }
    renderer_result = std::move(loaded.value());
    NEKO_LOG_INFO("renderer child loaded document title: " + renderer_result->title);
  } else if (parsed.options.url.has_value()) {
    const neko::base::Result<void> loaded =
        LoadTarget(page, parsed.options.url.value(), &local_storage, &indexed_db);
    if (!loaded) {
      std::cerr << "error: " << loaded.error().message() << "\n";
      return 1;
    }
    NEKO_LOG_INFO("loaded document title: " + page.document()->Title());
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
      neko::paint::Rasterizer image = page.Rasterize(800, height);
      const auto written = neko::paint::WritePpm(parsed.options.screenshot_path.value(), image);
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
