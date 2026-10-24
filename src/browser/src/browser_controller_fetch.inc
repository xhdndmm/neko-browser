// ---------------------------------------------------------------------------
// Renderer-process mode (ADR 0016 M2)
// ---------------------------------------------------------------------------

void BrowserController::LoadHtmlInRenderer(Tab& tab,
                                           std::string_view bytes,
                                           std::string_view content_type,
                                           const std::string& final_url,
                                           const std::string& origin)
{
  int viewport_width = kDefaultRemoteViewportWidth;
  int viewport_height = kDefaultRemoteViewportHeight;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tab.viewport_width > 0) {
      viewport_width = tab.viewport_width;
    }
    if (tab.viewport_height > 0) {
      viewport_height = tab.viewport_height;
    }
  }

  // Sessions are per site: the tab's child is reused while it serves the same
  // origin (same-site navigation reuses the process, per ADR 0016 M2) and
  // replaced when the site changes or the previous child died.
  std::shared_ptr<RendererSession> session;
  if (tab.session != nullptr && tab.session->alive() && tab.session_origin == origin) {
    session = tab.session;
  } else {
    // Shut the old child down before starting the new one (outside the lock:
    // the destructor reaps the process).
    std::shared_ptr<RendererSession> retired = std::move(tab.session);
    auto spawned = RendererSession::Spawn(renderer_executable_, origin);
    retired.reset();
    if (!spawned.has_value()) {
      MarkSessionFailed(tab, spawned.error().message());
      return;
    }
    session = spawned.value();
    std::lock_guard<std::mutex> lock(mutex_);
    tab.session = session;
    tab.session_origin = origin;
    tab.remote_frame.reset();
    tab.remote_content_height = 0;
    tab.remote_scroll_dirty = false;
    tab.remote_last_frame_ms = 0;
    tab.scroll_offset_y = 0;
    tab.pending_scroll_y = 0;
  }

  auto reply = session->Load(bytes, content_type, final_url, viewport_width, viewport_height);
  if (!reply.has_value()) {
    MarkSessionFailed(tab, reply.error().message());
    return;
  }
  if (tab.zoom != 1.0F) {
    // A zoomed tab keeps its zoom across navigations (browser behavior); the
    // child clamps and re-lays out, so the frame pulled below is scaled.
    auto zoomed = session->SetZoom(tab.zoom);
    if (!zoomed.has_value()) {
      MarkSessionFailed(tab, zoomed.error().message());
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.content_type = ContentType::kHtml;
    tab.error.reset();
    // The document lives in the child: no in-process page or script runtime.
    tab.page.reset();
    tab.script_runtime.reset();
    // A previous find-in-page session described the old document; the child's
    // list starts empty for the new one.
    tab.find_query.clear();
    tab.find_matches.clear();
    tab.find_index = -1;
    tab.find_match = renderer::FindMatch{};
    tab.find_match_count = 0;
    tab.title = reply.value().title.empty() ? std::string("Untitled") : reply.value().title;
    tab.remote_content_height = reply.value().content_height;
    tab.remote_hover_link = reply.value().hover_link;
  }
  // The first frame of the new document.
  PullRemoteFrame(tab, /*force=*/true);
  RecordVisit(final_url, tab.title);
}

bool BrowserController::ApplyRendererUpdate(Tab& tab, const RendererUpdate& update)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!update.title.empty()) {
      tab.title = update.title;
    }
    tab.remote_content_height = update.content_height;
    tab.remote_hover_link = update.hover_link;
    // The child's script-requested scroll latch drives the GUI scroll bar
    // through the same code path the in-process latch uses.
    if (update.scroll_request_id != 0) {
      tab.scroll_request_id = update.scroll_request_id;
      tab.pending_scroll_y = update.pending_scroll_y;
    }
  }
  // A navigation that happened inside the page is re-run here, so cookies,
  // history and content routing stay browser-side.
  if (!update.redirect_url.empty()) {
    (void)Navigate(tab.id, update.redirect_url);
    return true;
  }
  if (update.changed) {
    // Interaction-driven changes repaint immediately; only scroll-driven
    // pulls are throttled and coalesced (see PullRemoteFrame callers).
    PullRemoteFrame(tab, /*force=*/true);
  }
  return true;
}

void BrowserController::PullRemoteFrame(Tab& tab, bool force)
{
  if (tab.session == nullptr || !tab.session->alive()) {
    return;
  }
  const int64_t now = NowMillis();
  if (!force && now - tab.remote_last_frame_ms < kRemoteFrameMinIntervalMs) {
    // Coalesce: the next pump pulls the frame at the latest offset.
    tab.remote_scroll_dirty = true;
    return;
  }
  const int width = tab.viewport_width > 0 ? tab.viewport_width : kDefaultRemoteViewportWidth;
  const int height = tab.viewport_height > 0 ? tab.viewport_height : kDefaultRemoteViewportHeight;
  RemoteFrame frame;
  auto reply = tab.session->Snapshot(width, height, tab.scroll_offset_y, &frame);
  if (!reply.has_value()) {
    MarkSessionFailed(tab, reply.error().message());
    return;
  }
  tab.remote_last_frame_ms = now;
  tab.remote_scroll_dirty = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.remote_content_height = reply.value().content_height;
    tab.remote_frame_scroll_y = reply.value().scroll_y;
    tab.remote_hover_link = reply.value().hover_link;
    if (frame.width > 0 && frame.height > 0 && !frame.rgba.empty()) {
      tab.remote_frame = std::make_shared<const RemoteFrame>(std::move(frame));
    }
  }
}

void BrowserController::MarkSessionFailed(Tab& tab, std::string_view message)
{
  std::shared_ptr<RendererSession> dead;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dead = std::move(tab.session);
    tab.remote_frame.reset();
    tab.remote_content_height = 0;
    tab.remote_hover_link.clear();
    tab.content_type = ContentType::kError;
    tab.error =
        std::make_shared<std::string>("renderer process unavailable: " + std::string(message));
    tab.title = "Renderer error";
    tab.loading = false;
  }
  // Reap the child outside the lock (the destructor waits for it).
  dead.reset();
  LogConsole("error", "renderer session failed: " + std::string(message));
}

// ---------------------------------------------------------------------------
// Fetch + content routing
// ---------------------------------------------------------------------------

void BrowserController::FetchAndLoad(Tab& tab,
                                     const url::Url& url,
                                     const std::string& initiator_site,
                                     std::string method,
                                     std::string body,
                                     std::string content_type)
{
  NavigationDepthGuard guard(20);
  if (guard.Exceeded()) {
    NEKO_LOG_WARNING("navigation chain too deep; stopped at " + url.Serialize());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kError;
      tab.error = std::make_shared<std::string>("navigation loop detected");
      tab.title = "Navigation loop";
    }
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  const int64_t now = NowUnix();
  // A top-level navigation: Lax cookies ride it, Strict ones do not when the
  // initiator is cross-site.
  RequestContext context;
  context.initiator_site = initiator_site;
  context.top_level_navigation = true;
  context.method = std::move(method);
  context.body = std::move(body);
  context.content_type = std::move(content_type);
  const storage::CookieSameSiteContext same_site{
      /*cross_site=*/!initiator_site.empty() && security::SiteForCookies(url) != initiator_site,
      /*top_level_navigation=*/true};
  const std::string cookie = CookieHeader(url, now, same_site);
  auto response = fetch_(url, cookie, context);
  const auto end = std::chrono::steady_clock::now();
  const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

  NetworkLogEntry entry;
  entry.method = context.method;
  entry.url = url.Serialize();
  entry.elapsed_ms = elapsed_ms;
  entry.timestamp = NowUnix();
  if (!response) {
    entry.error = response.error().message();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      network_log_.push_back(entry);
      tab.content_type = ContentType::kError;
      tab.error = std::make_shared<std::string>("network error: " + response.error().message());
      tab.title = "Network error";
    }
    LogConsole("error", "failed to fetch " + url.Serialize() + ": " + response.error().message());
    return;
  }

  entry.status = response.value().status_code;
  entry.bytes = static_cast<int64_t>(response.value().body.size());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    network_log_.push_back(entry);
  }

  // Consume Set-Cookie headers.
  for (const network::HttpHeader& header : response.value().headers) {
    if (header.name == "set-cookie") {
      cookies_.SetCookieFromHeader(url, header.value, NowUnix());
    }
  }

  if (response.value().status_code >= 400) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kError;
      tab.error = std::make_shared<std::string>(
          "HTTP " + std::to_string(response.value().status_code) + " " + response.value().reason);
      tab.title = "HTTP " + std::to_string(response.value().status_code);
    }
    LogConsole("warning", *tab.error);
    return;
  }

  const std::string response_content_type = response.value().GetHeader("content-type");
  // Use the final URL (after redirects) so relative hrefs/srcs in the page
  // resolve against the real document URL, not the pre-redirect request URL.
  const std::string& final_url =
      response.value().final_url.empty() ? url.Serialize() : response.value().final_url;
  LoadBytes(tab, response.value().body, response_content_type, final_url);
}

void BrowserController::LoadBytes(Tab& tab,
                                  std::string_view bytes,
                                  std::string_view content_type,
                                  const std::string& final_url)
{
  const std::string ct = base::ToLower(content_type);

  // The security origin of the loaded content (Phase 10 M1): the tuple of the
  // final URL's scheme/host/port, "null" for non-URL content.  This is the
  // basis for the Same-Origin Policy (enforcement is future work).
  // The assignment is guarded by the controller mutex: the GUI reads snapshots
  // under the same lock (see ToSnapshot), so every Tab field write must be
  // locked even on the worker thread.
  const auto parsed = url::Url::Parse(final_url);
  const std::string origin = parsed.has_value()
                                 ? security::Origin::FromUrl(parsed.value()).Serialize()
                                 : std::string("null");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.origin = origin;
  }

  const bool is_image = ct.find("image/") != std::string::npos || neko::image::IsPng(bytes) ||
                        neko::image::IsJpeg(bytes);
  const bool is_pdf = ct.find("pdf") != std::string::npos || neko::pdf::IsPdf(bytes);
  const bool is_audio = ct.find("audio/") != std::string::npos || neko::media::IsWav(bytes);
  const bool is_text = ct.find("text/") != std::string::npos;
  const bool is_html = ct.find("html") != std::string::npos ||
                       ct.find("xml") != std::string::npos ||
                       (ct.empty() && !is_image && !is_pdf && !is_audio && LooksLikeHtml(bytes));

  if (is_html) {
    // Renderer-process mode (ADR 0016 M2): the document is parsed, styled,
    // laid out, painted and scripted in a child process.  The browser fetched
    // it (cookies are browser-side) and ships the bytes over the session.
    if (renderer_.enabled) {
      LoadHtmlInRenderer(tab, bytes, content_type, final_url, origin);
      return;
    }
    // Build the new page entirely on the worker thread and publish it in one
    // atomic step: a published Page (and its payload) is never mutated by the
    // worker afterwards, so the GUI can safely hold a shared handle and
    // Layout/Rasterize/hit-test it at any time.
    auto new_page = std::make_shared<renderer::Page>();
    // The HTTP Content-Type charset (if any) is the sniffing algorithm's
    // transport-layer hint; the BOM and <meta charset> still take precedence
    // inside Page::LoadHtml.
    const std::optional<base::encoding::Charset> http_charset =
        base::encoding::CharsetFromHttpHeader(content_type);
    auto r = new_page->LoadHtml(bytes, http_charset.value_or(base::encoding::Charset::kUnknown));
    if (!r) {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kError;
      tab.error = std::make_shared<std::string>("HTML parse error: " + r.error().message());
      tab.title = "Parse error";
      return;
    }
    // Phase 8 M2: execute the page's <script> elements (inline text and
    // external src=) with DOM bindings, fetching external scripts through the
    // same fetch path (with cookies).  Scripts may mutate the DOM;
    // RunPageScripts re-applies styles inside.  Console output from scripts
    // goes to DevTools' console log.  Phase 8 M3: the page's JS also gets
    // localStorage (scoped to the page origin) and fetch.
    browser::PageScriptServices services;
    services.local_storage = &local_storage_;
    // window.sessionStorage is tab-scoped: values survive navigation within
    // this tab and vanish when it is closed (WHATWG HTML 7.1).
    services.session_storage = &tab.session_storage;
    services.indexed_db = &indexed_db_;
    services.cookies = &cookies_;
    services.origin = origin;
    // Seed the script-visible session history (window.history) with the loaded
    // URL; the page's pushState/replaceState extend it.  Worker-thread only.
    tab.script_history = {final_url};
    tab.script_history_index = 0;
    tab.script_history_state.clear();
    services.script_history = &tab.script_history;
    services.script_history_index = &tab.script_history_index;
    services.script_history_state = &tab.script_history_state;
    // Scroll bridging: the tab's offset is read live (window.scrollY) and a
    // script-requested scroll bumps the GUI-visible latch for the WebView to
    // apply to its scroll bar.  [this] and &tab are worker-thread only.
    services.scroll_offset_y = &tab.scroll_offset_y;
    services.set_scroll_request = [this, &tab](int, float y) { SetTabScrollRequest(tab.id, y); };
    // Focus control (WHATWG HTML §6.6): Element.focus()/blur(),
    // document.activeElement and document.hasFocus() go through the browser's
    // focus state -- the same machinery clicks, Tab and autofocus use.  The
    // page is not published on the tab yet, so these callbacks target
    // |new_page| explicitly; a script may focus during the load, and
    // ApplyAutofocus below then leaves that focus alone.  They never dispatch
    // the focus events themselves: the binder that called focus() runs the UI
    // Events sequence, so handlers fire in the calling realm.  [&tab] and
    // |new_page| are worker-thread only, like the binder they outlive.
    services.focus_element = [&tab, new_page](dom::Element& element) {
      if (!CanFocus(element, new_page.get())) {
        return false;
      }
      if (tab.focused_element != &element) {
        SetFocusState(&tab, &element, new_page.get());
        // Script focus behaves like keyboard focus: a text control's contents
        // are selected (browsers' focus()/select() behavior).
        FocusTextControlSelection(element, /*select_contents=*/true);
      }
      return true;
    };
    services.blur_element = [&tab, new_page](dom::Element& element) {
      if (tab.focused_element != &element) {
        return false;
      }
      SetFocusState(&tab, nullptr, new_page.get());
      return true;
    };
    services.active_element = [&tab, new_page]() -> dom::Element* {
      const dom::Document* document = new_page->document();
      if (tab.focused_element == nullptr || document == nullptr ||
          !IsInDocument(*tab.focused_element, *document)) {
        return nullptr;
      }
      return tab.focused_element;
    };
    services.has_focus = [this, &tab]() { return ActiveTab() == &tab; };
    // The page's own site: these are subresource requests initiated by it.
    RequestContext subresource_context;
    if (const base::Result<url::Url> page_url = url::Url::Parse(final_url); page_url.has_value()) {
      subresource_context.initiator_site = security::SiteForCookies(page_url.value());
    }
    const auto fetch_subresource = [this, subresource_context](const url::Url& resource_url,
                                                               std::string_view,
                                                               const RequestContext&) {
      const storage::CookieSameSiteContext same_site{
          /*cross_site=*/!subresource_context.initiator_site.empty() &&
              security::SiteForCookies(resource_url) != subresource_context.initiator_site,
          /*top_level_navigation=*/false};
      return fetch_(
          resource_url, CookieHeader(resource_url, NowUnix(), same_site), subresource_context);
    };
    // Fetch and apply the page's external <link rel=stylesheet> sheets before
    // scripts run, so scripts see the fully styled cascade.
    FetchExternalStylesheets(*new_page, final_url, fetch_subresource, *pool_);
    // The navigation request lives in the tab (stable address): the runtime's
    // callback keeps writing into it when a timer navigates later.
    tab.pending_script_navigation = {};
    tab.pending_form_submission = {};
    tab.script_runtime = RunPageScripts(
        *new_page,
        final_url,
        [this, subresource_context](const url::Url& script_url, const RequestContext&) {
          const storage::CookieSameSiteContext same_site{
              /*cross_site=*/!subresource_context.initiator_site.empty() &&
                  security::SiteForCookies(script_url) != subresource_context.initiator_site,
              /*top_level_navigation=*/false};
          return fetch_(
              script_url, CookieHeader(script_url, NowUnix(), same_site), subresource_context);
        },
        [this](std::string_view level, std::string_view text) { LogConsole(level, text); },
        pool(),
        services,
        &tab.pending_script_navigation,
        /*timings=*/nullptr,
        &tab.pending_form_submission);

    // A script may have called form.submit()/requestSubmit() while the
    // document was loading (auto-submitting redirect pages).  Run the
    // submission against the not-yet-published document: if it navigates, it
    // replaces this document before it is ever shown.
    if (tab.pending_form_submission.form != nullptr) {
      const ScriptRequestedFormSubmission request = tab.pending_form_submission;
      tab.pending_form_submission = {};
      const bool navigated = RunFormSubmission(
          tab, *new_page, final_url, request.form, request.submitter, !request.request_submit);
      if (navigated) {
        return;
      }
    }

    // A script may have requested a navigation (window.location.href=,
    // assign()/replace(), or reload()) — e.g. Baidu's anti-bot page replaces
    // the URL.  Act on it instead of publishing the script's own document;
    // the requested navigation is already resolved to an absolute URL.
    const std::string requested_url = tab.pending_script_navigation.url;
    const bool requested_reload = tab.pending_script_navigation.is_reload;
    tab.pending_script_navigation = {};
    if (!requested_url.empty()) {
      NavigateToUrl(tab, requested_url);
      return;
    }
    if (requested_reload && !tab.url.empty()) {
      NavigateToUrl(tab, tab.url);
      return;
    }

    // Password manager: fill a saved login into the page's form before it is
    // published, so the first frame already shows it.  The renderer-process
    // path returned above (the child owns the DOM in that mode).
    AutofillLoginForms(*new_page, origin);

    // Autofocus processing (HTML §6.6.1) runs once the document has loaded and
    // after every navigation decision was taken; a script that focused another
    // element during the load wins (ApplyAutofocus checks the tab state).
    ApplyAutofocus(tab, *new_page);

    // Publish first so the UI paints text immediately; <img>/<video>
    // subresources load on the pool afterwards and pop in through the
    // page-version invalidation + periodic refresh (a single failing CDN
    // request used to block first paint for its whole connect timeout).
    std::string title = new_page->document()->Title();
    if (title.empty())
      title = final_url;
    if (tab.zoom != 1.0F) {
      // A zoomed tab keeps its zoom across navigations (browser behavior).
      new_page->SetUserZoom(tab.zoom);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kHtml;
      tab.page = new_page; // shared: the background task keeps it alive
      // A fresh document must produce a fresh viewport frame (ADR 0020).
      tab.frame_dirty = true;
      tab.title = std::move(title);
      // A previous find-in-page session described the old document.
      tab.find_query.clear();
      tab.find_matches.clear();
      tab.find_index = -1;
      tab.find_match = renderer::FindMatch{};
      tab.find_match_count = 0;
      // Reflect a script history.pushState/replaceState in the address bar.
      // Only when the page actually pushed/replaced (the ended-on entry differs
      // from the loaded URL): otherwise leave the controller's own tab.url
      // (e.g. a bare file path) untouched.
      if (!tab.script_history.empty() && tab.script_history_index < tab.script_history.size() &&
          tab.script_history[tab.script_history_index] != final_url) {
        tab.url = tab.script_history[tab.script_history_index];
      }
    }
    RecordVisit(final_url, tab.title);

    // Web fonts are allowed to arrive after first paint. The page is shared
    // with this task, and Page synchronizes font registration and layout
    // invalidation for the UI thread.
    // Worker-thread actions run serially, so no navigation can replace
    // tab.page behind our back while this task runs.
    if (pool_->thread_count() >= 4) {
      // Each loader uses the remaining workers for per-URL futures. With the
      // normal hardware-sized pool there are enough workers for all three
      // outer tasks and their nested URL work to make progress together.
      pool_->Post(
          [page = std::shared_ptr(new_page), final_url, fetch_subresource, &pool = *pool_]() {
            FetchWebFonts(*page, final_url, fetch_subresource, pool);
          });
      pool_->Post(
          [page = std::shared_ptr(new_page), final_url, fetch_subresource, &pool = *pool_]() {
            FetchPageImages(*page, final_url, fetch_subresource, pool);
          });
      pool_->Post(
          [page = std::shared_ptr(new_page), final_url, fetch_subresource, &pool = *pool_]() {
            FetchPageVideos(*page, final_url, fetch_subresource, pool);
          });
    } else if (pool_->thread_count() >= 2) {
      // With fewer workers, one outer task avoids nested-future starvation.
      pool_->Post(
          [page = std::shared_ptr(new_page), final_url, fetch_subresource, &pool = *pool_]() {
            FetchWebFonts(*page, final_url, fetch_subresource, pool);
            FetchPageImages(*page, final_url, fetch_subresource, pool);
            FetchPageVideos(*page, final_url, fetch_subresource, pool);
          });
    } else {
      // Single-worker pool: decoding inside a pool task would deadlock on
      // its own futures — keep the old inline path.
      FetchPageImages(*new_page, final_url, fetch_subresource, *pool_);
      FetchPageVideos(*new_page, final_url, fetch_subresource, *pool_);
    }
    return;
  }

  if (is_image) {
    // An animated GIF navigated to directly plays like any other image in a
    // browser: decode every frame and let the frame clock advance the display.
    std::shared_ptr<image::GifAnimation> animation;
    if (image::IsGif(bytes)) {
      auto decoded_animation = image::DecodeGifAnimation(bytes);
      if (decoded_animation.has_value() && decoded_animation.value().frames.size() > 1) {
        animation = std::make_shared<image::GifAnimation>(std::move(decoded_animation.value()));
      }
    }
    image::Image decoded;
    if (animation != nullptr) {
      decoded.width = animation->width;
      decoded.height = animation->height;
      decoded.rgba = animation->frames[0].rgba;
    } else {
      // Direct SVG navigation: decode with the tab's font stack so <text>
      // draws.  (A single-frame GIF falls through here.)
      const image::SvgTextShaper shaper =
          tab.page != nullptr ? tab.page->MakeSvgTextShaper() : image::SvgTextShaper{};
      auto result = neko::image::DecodeImage(bytes, shaper);
      if (!result) {
        std::lock_guard<std::mutex> lock(mutex_);
        tab.content_type = ContentType::kError;
        tab.error =
            std::make_shared<std::string>("image decode error: " + result.error().message());
        tab.title = "Image error";
        return;
      }
      decoded = std::move(result.value());
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kImage;
      tab.image = std::make_shared<image::Image>(std::move(decoded));
      tab.gif_animation = std::move(animation);
      tab.gif_frame = 0;
      tab.gif_loops = 0;
      tab.gif_finished = false;
      // Playback starts when the image is published (not on the first pump):
      // the first frame's delay must elapse in real time.
      tab.gif_started = true;
      tab.gif_start_ms = static_cast<double>(NowMillis());
      tab.image_frame = 0;
      tab.title = final_url;
    }
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_pdf) {
    auto pdf_result = neko::pdf::ExtractText(bytes);
    if (!pdf_result) {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kError;
      tab.error = std::make_shared<std::string>("pdf error: " + pdf_result.error().message());
      tab.title = "PDF error";
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kPdf;
      tab.pdf = std::make_shared<pdf::PdfDocument>(std::move(pdf_result.value()));
      tab.title = tab.pdf->title.empty() ? final_url : tab.pdf->title;
    }
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_audio) {
    auto decoded = neko::media::DecodeWav(bytes);
    if (!decoded) {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kError;
      tab.error = std::make_shared<std::string>("audio decode error: " + decoded.error().message());
      tab.title = "Audio error";
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kAudio;
      tab.audio = std::make_shared<media::AudioData>(std::move(decoded.value()));
      tab.title = final_url;
    }
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_text) {
    // Decode legacy-encoded text (e.g. GBK) into UTF-8 so the viewer shows
    // readable text instead of mojibake.
    const std::optional<base::encoding::Charset> charset =
        base::encoding::CharsetFromHttpHeader(content_type);
    const std::string utf8 =
        base::encoding::DecodeToUtf8(bytes, charset.value_or(base::encoding::Charset::kUtf8));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kText;
      tab.raw_text = std::make_shared<std::string>(utf8);
      tab.title = final_url;
    }
    RecordVisit(final_url, tab.title);
    return;
  }

  // Unknown binary content.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.content_type = ContentType::kOther;
    tab.raw_text = std::make_shared<std::string>(bytes);
    tab.title = final_url;
  }
  RecordVisit(final_url, tab.title);
}

void BrowserController::RecordVisit(const std::string& url, const std::string& title)
{
  history_.RecordVisit(url, title, NowUnix());
}

