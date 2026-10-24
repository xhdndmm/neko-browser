// BrowserController: construction, destruction and the small public helpers.
//
// One class, several responsibilities, split into separate translation units
// that share browser_controller_internal.h:
//   browser_controller_tabs.cpp        tab lifecycle + GUI snapshots
//   browser_controller_navigation.cpp  navigation + input dispatch
//   browser_controller_forms.cpp       form submission
//   browser_controller_frames.cpp      frame production, timers, image events
//   browser_controller_view.cpp        scroll, zoom, find-in-page
//   browser_controller_fetch.cpp       renderer mode + content routing
//   browser_controller_storage.cpp     bookmarks, persistence, DevTools
//   browser_controller_helpers.cpp     module-internal helpers
#include "neko/browser/browser_controller.h"

#include "neko/base/logging.h"
#include "neko/base/memory.h"
#include "neko/base/string_util.h"
#include "neko/base/thread_pool.h"
#include "neko/browser/hyperlink.h"
#include "neko/browser/page_scripts.h"
#include "neko/browser/preferences_keys.h"
#include "neko/browser/renderer_host.h"
#include "neko/browser/search_engines.h"
#include "neko/css/parser.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/html/form_validation.h"
#include "neko/html/input_value.h"
#include "neko/image/image.h"
#include "neko/layout/form_control.h"
#include "neko/network/http.h"
#include "neko/paint/rasterizer.h"
#include "neko/platform/executable_path.h"
#include "neko/security/origin.h"
#include "neko/security/site.h"
#include "neko/storage/file_util.h"
#include "neko/url/url.h"

#include "browser_controller_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <future>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neko::browser {

std::string_view ToString(ContentType type)
{
  switch (type) {
  case ContentType::kHtml:
    return "html";
  case ContentType::kImage:
    return "image";
  case ContentType::kPdf:
    return "pdf";
  case ContentType::kAudio:
    return "audio";
  case ContentType::kText:
    return "text";
  case ContentType::kOther:
    return "other";
  case ContentType::kError:
    return "error";
  }
  return "unknown";
}

BrowserController::BrowserController(std::string profile_dir,
                                     FetchFn fetch,
                                     RendererOptions renderer,
                                     NetworkOptions network)
    : profile_dir_(std::move(profile_dir)), fetch_(std::move(fetch)),
      renderer_(std::move(renderer)), network_(std::move(network)), cookies_(profile_dir_),
      history_(profile_dir_), bookmarks_(profile_dir_), local_storage_(profile_dir_),
      indexed_db_(profile_dir_), preferences_(profile_dir_), passwords_(profile_dir_),
      downloads_(profile_dir_ + "/downloads")
{
  if (renderer_.enabled) {
    renderer_executable_ =
        renderer_.executable.empty() ? DefaultRendererExecutable() : renderer_.executable;
    if (renderer_executable_.empty()) {
      NEKO_LOG_WARNING("renderer process mode requested but no renderer executable was found; "
                       "falling back to in-process rendering");
      renderer_.enabled = false;
    }
  }
  pool_ = std::make_unique<base::ThreadPool>();
  // The raster pool: up to four workers (HardwareConcurrency() - 1), and one
  // worker means RasterizeParallel falls back to the serial path -- which is
  // exactly what a two-core machine wants, and never waits on the shared
  // pool's capacity.
  {
    const std::size_t hardware = base::ThreadPool::HardwareConcurrency();
    const std::size_t raster_threads = hardware <= 2 ? 1 : std::min<std::size_t>(4, hardware - 1);
    raster_pool_ = std::make_unique<base::ThreadPool>(raster_threads);
  }
  // Default fetch: compute cookies for each redirect hop from the controller's
  // cookie jar.  HttpGet invokes HeaderProvider with the current hop URL.
  // With network-process mode the whole fetch runs in a child; the child asks
  // back for each hop's cookies, so the jar (and HttpOnly values) stay here.
  if (!fetch_) {
    fetch_ = [this](const url::Url& u, std::string_view, const RequestContext& context) {
      // SameSite is decided per hop against the initiator's site (a redirect
      // target may differ from the original target).
      const auto same_site_for = [&context](const url::Url& target) {
        storage::CookieSameSiteContext same_site;
        same_site.top_level_navigation = context.top_level_navigation;
        same_site.cross_site = !context.initiator_site.empty() &&
                               security::SiteForCookies(target) != context.initiator_site;
        return same_site;
      };
      if (network_.enabled) {
        NetworkSession* session = EnsureNetworkSession();
        if (session != nullptr) {
          NetworkSession::CookieLookupFn lookup = [this, &same_site_for](const url::Url& target) {
            return CookieHeader(target, NowUnix(), same_site_for(target));
          };
          // NOTE: extra_headers do not cross the network-process protocol yet
          // (the request serializer carries url, cookie, method and body); the
          // CORS Origin header is therefore absent there.  That fails CLOSED
          // (an Origin-varying server grants no ACAO -> the response is
          // refused), so it is a compatibility gap, not a security hole.
          auto fetched = session->Fetch(
              u, /*cookie_header=*/{}, lookup, context.method, context.body, context.content_type);
          if (fetched.has_value()) {
            return fetched;
          }
          NEKO_LOG_WARNING("network process fetch failed; falling back in-process: " +
                           fetched.error().message());
        }
      }
      network::HeaderProvider provider;
      provider = [this, &context, &same_site_for](const url::Url& target) {
        std::vector<network::HttpHeader> headers;
        headers.reserve(context.extra_headers.size() + 1);
        for (const auto& [name, value] : context.extra_headers) {
          headers.push_back({name, value});
        }
        const std::string cookie = CookieHeader(target, NowUnix(), same_site_for(target));
        if (!cookie.empty()) {
          headers.push_back({"cookie", cookie});
        }
        return headers;
      };
      // Form submission posts an application/x-www-form-urlencoded body; every
      // other fetch in the engine is a GET (the default context method).
      if (context.method == "POST") {
        return network::HttpPost(u, context.body, context.content_type, 5, provider);
      }
      return network::HttpGet(u, 5, provider);
    };
  }
}

NetworkSession* BrowserController::EnsureNetworkSession()
{
  if (!network_.enabled) {
    return nullptr;
  }
  if (network_session_ && network_session_->alive()) {
    return network_session_.get();
  }
  const std::string executable =
      network_.executable.empty() ? DefaultRendererExecutable() : network_.executable;
  if (executable.empty()) {
    NEKO_LOG_WARNING("network process mode requested but no executable was found");
    return nullptr;
  }
  auto spawned = NetworkSession::Spawn(executable);
  if (!spawned.has_value()) {
    NEKO_LOG_WARNING("network process failed to start: " + spawned.error().message());
    network_session_.reset();
    return nullptr;
  }
  network_session_ = spawned.value();
  return network_session_.get();
}

BrowserController::~BrowserController()
{
  (void)Save();
}

} // namespace neko::browser
