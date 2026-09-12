#include "neko/browser/renderer_session_host.h"

#include "neko/base/logging.h"
#include "neko/browser/browser_controller.h"
#include "neko/browser/hyperlink.h"
#include "neko/browser/renderer_protocol.h"
#include "neko/dom/element.h"
#include "neko/ipc/channel.h"
#include "neko/paint/rasterizer.h"
#include "neko/renderer/page.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace neko::browser {
namespace {

long CurrentProcessId()
{
#ifdef _WIN32
  return static_cast<long>(::_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

// Serves one renderer session: a controller with a single tab whose documents
// arrive over the channel as bytes.  The child never fetches top-level
// documents; a navigation triggered inside the page (link click, script) is
// reported back as a redirect for the browser to re-run with its cookies.
class SessionHost
{
public:
  SessionHost(ipc::Channel channel, std::string profile_dir)
      : channel_(std::move(channel)), profile_dir_(std::move(profile_dir))
  {}

  int Run()
  {
    controller_ = std::make_unique<BrowserController>(profile_dir_);
    tab_id_ = controller_->NewTab();
    while (!shutdown_) {
      auto frame = channel_.Receive();
      if (!frame.has_value()) {
        break; // The browser closed the pipe (or died): shut down.
      }
      RendererSessionReply reply;
      auto request = DecodeSessionRequest(frame.value());
      if (!request.has_value()) {
        reply.ok = false;
        reply.error = request.error().message();
      } else {
        reply = HandleOp(request.value());
      }
      const auto encoded = EncodeSessionReply(reply);
      if (!encoded.has_value() || !channel_.Send(encoded.value())) {
        break;
      }
    }
    // Destroy the controller before the caller removes the temp profile (its
    // destructor saves the profile stores).
    controller_.reset();
    return 0;
  }

private:
  renderer::Page* page()
  {
    Tab* tab = controller_->FindTab(tab_id_);
    if (tab == nullptr || tab->content_type != ContentType::kHtml) {
      return nullptr;
    }
    return tab->page.get();
  }

  // Lays the page out when it has no layout yet or changed since the last
  // layout (DOM/style mutation, new document, viewport resize).
  void EnsureLayout()
  {
    renderer::Page* p = page();
    if (p == nullptr) {
      return;
    }
    if (p->HasLayout() && p->layout_version() == laid_out_layout_version_ &&
        p->DocumentVersion() == laid_out_document_version_ && laid_out_width_ == viewport_width_ &&
        laid_out_height_ == viewport_height_) {
      return;
    }
    p->Layout(static_cast<float>(viewport_width_), static_cast<float>(viewport_height_));
    laid_out_layout_version_ = p->layout_version();
    laid_out_document_version_ = p->DocumentVersion();
    laid_out_width_ = viewport_width_;
    laid_out_height_ = viewport_height_;
  }

  // Updates the :hover rendering hint and the hyperlink under the pointer
  // (mirrors what the in-process GUI does against its own page).
  void UpdateHoverHint(float doc_x, float doc_y)
  {
    renderer::Page* p = page();
    if (p == nullptr) {
      hover_link_.clear();
      return;
    }
    const dom::Element* element = p->ElementAt(doc_x, doc_y);
    p->SetHoveredElement(element);
    Tab* tab = controller_->FindTab(tab_id_);
    const auto target =
        element != nullptr && tab != nullptr ? HyperlinkTarget(element, tab->url) : std::nullopt;
    hover_link_ = target.value_or(std::string());
  }

  // Fills the common status fields (URL, title, scroll latches, content
  // height, changed flag).  |report_changed| is false for snapshot replies,
  // which always carry fresh pixels.
  void FillStatus(RendererSessionReply& reply, bool report_changed)
  {
    const TabSnapshot snapshot = controller_->SnapshotTab(tab_id_);
    reply.ok = true;
    reply.url = snapshot.url;
    reply.title = snapshot.title;
    reply.scroll_request_id = snapshot.scroll_request_id;
    reply.pending_scroll_y = snapshot.pending_scroll_y;
    Tab* tab = controller_->FindTab(tab_id_);
    if (tab != nullptr) {
      reply.scroll_y = tab->scroll_offset_y;
    }
    reply.hover_link = hover_link_;

    renderer::Page* p = page();
    if (p == nullptr) {
      reply.content_height = 0;
      reply.changed = report_changed;
      return;
    }
    EnsureLayout();
    reply.content_height = p->ContentHeight();
    const std::uint64_t layout_version = p->layout_version();
    const std::uint64_t document_version = p->DocumentVersion();
    const bool changed =
        layout_version != seen_layout_version_ || document_version != seen_document_version_;
    seen_layout_version_ = layout_version;
    seen_document_version_ = document_version;
    reply.changed = report_changed && changed;
  }

  RendererSessionReply HandleOp(const RendererSessionRequest& request)
  {
    RendererSessionReply reply;
    switch (request.op) {
    case SessionOp::kLoad: {
      viewport_width_ = std::max(1, request.viewport_width);
      viewport_height_ = std::max(1, request.viewport_height);
      laid_out_width_ = -1;
      laid_out_height_ = -1;
      // A new document always counts as a change.
      seen_layout_version_ = 0;
      seen_document_version_ = 0;
      hover_link_.clear();
      const auto loaded =
          controller_->LoadDocument(tab_id_, request.document, request.content_type, request.url);
      if (!loaded.has_value()) {
        reply.ok = false;
        reply.error = loaded.error().message();
        return reply;
      }
      // The browser issued this load, so it knows the resulting URL; only a
      // later divergence counts as a page-initiated navigation.
      navigation_baseline_ = controller_->SnapshotTab(tab_id_).url;
      FillStatus(reply, /*report_changed=*/true);
      reply.changed = true;
      return reply;
    }
    case SessionOp::kClick:
      reply.handled = controller_->DispatchPointerClick(tab_id_, request.x, request.y);
      break;
    case SessionOp::kHover:
      controller_->DispatchHover(tab_id_, request.x, request.y);
      UpdateHoverHint(request.x, request.y);
      break;
    case SessionOp::kHoverClear:
      controller_->DispatchHoverClear(tab_id_);
      if (renderer::Page* p = page(); p != nullptr) {
        p->SetHoveredElement(nullptr);
      }
      hover_link_.clear();
      break;
    case SessionOp::kWheel:
      reply.handled = controller_->DispatchWheel(tab_id_, request.delta_y);
      break;
    case SessionOp::kKey:
      reply.handled =
          controller_->DispatchKeyboard(tab_id_, request.key_type, request.key, request.code);
      break;
    case SessionOp::kScroll:
      controller_->SetTabScrollOffset(tab_id_, request.scroll_y);
      break;
    case SessionOp::kPump:
      controller_->PumpScriptTimers();
      break;
    case SessionOp::kSnapshot: {
      if (request.viewport_width > 0 && request.viewport_height > 0) {
        viewport_width_ = std::max(1, request.viewport_width);
        viewport_height_ = std::max(1, request.viewport_height);
      }
      renderer::Page* p = page();
      if (p == nullptr) {
        reply.ok = false;
        reply.error = "no document to rasterize";
        return reply;
      }
      EnsureLayout();
      const float max_scroll =
          std::max(0.0f, p->ContentHeight() - static_cast<float>(viewport_height_));
      const float scroll = std::clamp(request.scroll_y, 0.0f, max_scroll);
      const paint::Rasterizer raster =
          p->Rasterize(viewport_width_, viewport_height_, scroll, &controller_->pool());
      reply.has_frame = true;
      reply.width = raster.width();
      reply.height = raster.height();
      reply.rgba = raster.pixels();
      FillStatus(reply, /*report_changed=*/false);
      reply.scroll_y = scroll;
      return reply;
    }
    case SessionOp::kShutdown:
      shutdown_ = true;
      reply.ok = true;
      return reply;
    }

    // Common tail for the interaction ops: a navigation that happened inside
    // the page (link default action, script) is handed back to the browser,
    // which owns cookies and content routing.
    Tab* tab = controller_->FindTab(tab_id_);
    if (tab != nullptr && tab->url != navigation_baseline_) {
      reply.redirect_url = tab->url;
    }
    FillStatus(reply, /*report_changed=*/true);
    if (tab != nullptr) {
      navigation_baseline_ = tab->url;
    }
    return reply;
  }

  ipc::Channel channel_;
  std::string profile_dir_;
  std::unique_ptr<BrowserController> controller_;
  int tab_id_ = 0;
  bool shutdown_ = false;

  // Viewport the page is laid out for (set by kLoad / kSnapshot).
  int viewport_width_ = 800;
  int viewport_height_ = 600;

  // Bookkeeping for EnsureLayout (last laid-out versions and size).
  std::uint64_t laid_out_layout_version_ = 0;
  std::uint64_t laid_out_document_version_ = 0;
  int laid_out_width_ = -1;
  int laid_out_height_ = -1;

  // Versions the browser last saw (drives the |changed| flag).
  std::uint64_t seen_layout_version_ = 0;
  std::uint64_t seen_document_version_ = 0;

  // The tab URL the browser believes in; a difference means the page
  // navigated on its own and the browser must re-run the navigation.
  std::string navigation_baseline_;

  // Hyperlink under the pointer (reported so the GUI can show a hand cursor
  // without the DOM).
  std::string hover_link_;
};

} // namespace

int RunRendererSession()
{
  ipc::Channel channel = ipc::Channel::FromHandles(0, 1);

  // Page storage in a renderer session is per-session scratch (the browser
  // owns the profile); it lives in a temp directory removed on exit.
  std::error_code ec;
  std::filesystem::path profile = std::filesystem::temp_directory_path(ec);
  if (profile.empty()) {
    profile = std::filesystem::current_path(ec);
  }
  profile /= "neko-renderer-" + std::to_string(CurrentProcessId());
  std::filesystem::create_directories(profile, ec);
  if (ec) {
    NEKO_LOG_WARNING("renderer session: failed to create the scratch profile: " + ec.message());
  }

  int exit_code = 0;
  {
    SessionHost host(std::move(channel), profile.string());
    exit_code = host.Run();
  }
  std::filesystem::remove_all(profile, ec);
  return exit_code;
}

} // namespace neko::browser
