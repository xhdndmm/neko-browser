base::Result<void> BrowserController::NavigateActive(const std::string& input)
{
  Tab* tab = ActiveTab();
  if (tab == nullptr)
    return base::Err(base::Error::InvalidArgument("no active tab"));
  return Navigate(tab->id, input);
}

base::Result<void> BrowserController::LoadDocument(int tab_id,
                                                   std::string_view bytes,
                                                   std::string_view content_type,
                                                   const std::string& final_url)
{
  // Loading replaces the tab's document; release the previous one's arena
  // pages when done (same reasoning as NavigateToUrl).
  [[maybe_unused]] const HeapTrimOnExit trim_heap_on_exit;

  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return base::Err(base::Error::InvalidArgument("no such tab"));
  }
  // The tab now shows this document: its URL is the document's base (relative
  // hrefs, window.location, form actions) and the old focus/hover state is
  // stale.  Mirrors the bookkeeping NavigateToUrl does for the fetch path.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab->url = final_url;
  }
  tab->focused_element = nullptr;
  tab->autofocus_processed.clear();
  tab->hovered_element = nullptr;
  tab->pending_form_submission = {};
  LoadBytes(*tab, bytes, content_type, final_url);
  if (tab->content_type == ContentType::kError && tab->error != nullptr) {
    return base::Err(base::Error::Unknown(*tab->error));
  }
  return base::Ok();
}

void BrowserController::Back()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr || !tab->CanGoBack())
    return;
  --tab->history_index;
  const std::string target = tab->history[static_cast<size_t>(tab->history_index)];
  NavigateToUrl(*tab, target);
}

void BrowserController::Forward()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr || !tab->CanGoForward())
    return;
  ++tab->history_index;
  const std::string target = tab->history[static_cast<size_t>(tab->history_index)];
  NavigateToUrl(*tab, target);
}

void BrowserController::Reload()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr || tab->url.empty())
    return;
  NavigateToUrl(*tab, tab->url);
}

void BrowserController::ResolveHover(Tab& tab)
{
  tab.hovered_element = nullptr;
  if (tab.page == nullptr || !tab.has_hover) {
    return;
  }
  tab.hovered_element = const_cast<dom::Element*>(tab.page->ElementAt(tab.hover_x, tab.hover_y));
}

std::size_t BrowserController::FirePendingImageEvents(Tab& tab)
{
  if (tab.page == nullptr) {
    return 0;
  }
  std::vector<std::pair<const dom::Element*, bool>> events = tab.page->TakePendingImageEvents();
  if (events.empty() || tab.script_runtime == nullptr) {
    // Without a binder there is nobody to notify; draining above still keeps
    // the queue bounded on script-less pages.
    return 0;
  }
  const std::shared_ptr<renderer::Page> page = tab.page;
  std::size_t fired = 0;
  for (const auto& [element, loaded] : events) {
    // A previous event's script may have navigated (page replaced) — and even
    // within the same document the element may have been removed meanwhile.
    if (tab.page != page || !page->ContainsElement(element)) {
      continue;
    }
    tab.script_runtime->DispatchNonBubblingEvent(*const_cast<dom::Element*>(element),
                                                 loaded ? "load" : "error");
    ++fired;
  }
  return fired;
}

void BrowserController::SchedulePendingImageFetch(Tab& tab)
{
  if (tab.page == nullptr) {
    return;
  }
  if (tab.image_fetch_state == nullptr) {
    tab.image_fetch_state = std::make_shared<Tab::ImageFetchState>();
  }
  const std::shared_ptr<Tab::ImageFetchState> state = tab.image_fetch_state;
  if (state->in_flight.load()) {
    // A pass is running: remember that one more is needed (a source may have
    // appeared after the running pass collected its claims).
    state->again.store(true);
    return;
  }
  state->again.store(false);
  state->in_flight.store(true);
  const std::shared_ptr<renderer::Page> page = tab.page;
  const std::string base_url = tab.url;
  // Same fetcher the load-time subresource pass uses (cookies follow the
  // resource URL).  [this] is bounded by the pool draining on destruction;
  // the state/page handles outlive the tab if it is navigated away.
  // The pass runs on the pool while the tab belongs to the worker: capture the
  // request context now (the page's site is the initiator for its subresources).
  RequestContext context;
  if (const base::Result<url::Url> page_url = url::Url::Parse(base_url); page_url.has_value()) {
    context.initiator_site = security::SiteForCookies(page_url.value());
  }
  const auto fetch = [this, context](
                         const url::Url& resource_url, std::string_view, const RequestContext&) {
    const storage::CookieSameSiteContext same_site{/*cross_site=*/!context.initiator_site.empty() &&
                                                       security::SiteForCookies(resource_url) !=
                                                           context.initiator_site,
                                                   /*top_level_navigation=*/false};
    return fetch_(resource_url, CookieHeader(resource_url, NowUnix(), same_site), context);
  };
  pool_->Post([state, page, base_url, fetch, pool = pool_.get()]() {
    do {
      FetchPageImages(*page, base_url, fetch, *pool);
    } while (state->again.exchange(false));
    state->in_flight.store(false);
    // This pass ran on a pool thread while the Tab belongs to the worker
    // thread, so nothing here may touch Tab state.  The results are published
    // as atomics and applied by the worker's next pump
    // (ApplyDeferredImageFetchSignals): a finished pass means the GUI owes a
    // repaint — the placeholder would otherwise stay gray until some
    // unrelated event repainted — and a source claimed while the pass ran
    // means one more pass is due.  The old code wrote tab->frame_dirty under
    // mutex_ from here, which raced the worker's DOM-lock-protected writes
    // to the same field (TSan: this lambda vs DispatchKeyboard).
    state->wake_frame.store(true);
    if (state->again.exchange(false)) {
      state->reschedule.store(true);
    }
  });
}

void BrowserController::ApplyDeferredImageFetchSignals(Tab& tab)
{
  if (tab.image_fetch_state == nullptr) {
    return;
  }
  const std::shared_ptr<Tab::ImageFetchState> state = tab.image_fetch_state;
  if (state->wake_frame.exchange(false)) {
    tab.frame_dirty = true;
  }
  if (state->reschedule.exchange(false)) {
    // A source was claimed while the last pass ran: fetch it now (a pass
    // already running would just set |again| again).
    SchedulePendingImageFetch(tab);
  }
}

void BrowserController::RecordFrameProduced(long long production_ms, long long now_ms)
{
  std::lock_guard<std::mutex> lock(stats_mutex_);
  ++frame_stats_.frames;
  frame_stats_.production.Record(production_ms);
  if (last_frame_ms_ != 0) {
    frame_stats_.interval.Record(now_ms - last_frame_ms_);
  }
  last_frame_ms_ = now_ms;
}

void BrowserController::SetRafPerTurnLimit(std::size_t limit)
{
  std::lock_guard<std::mutex> lock(stats_mutex_);
  raf_per_turn_limit_ = limit;
}

RuntimeStats BrowserController::SnapshotRuntimeStats() const
{
  std::lock_guard<std::mutex> lock(stats_mutex_);
  RuntimeStats stats;
  stats.pump = pump_turn_stats_;
  stats.frames = frame_stats_;
  return stats;
}

void BrowserController::ProduceFrame(Tab& tab)
{
  const auto frame_start = std::chrono::steady_clock::now();
  ProduceFrameBody(tab);
  // Count only frames that were actually produced (an empty or non-HTML tab
  // clears the frame instead).
  if (tab.frame != nullptr) {
    const auto now = std::chrono::steady_clock::now();
    RecordFrameProduced(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - frame_start).count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
  }
}

void BrowserController::ProduceFrameBody(Tab& tab)
{
  if (tab.content_type != ContentType::kHtml || tab.page == nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.frame = nullptr;
    tab.frame_content_height = 0;
    tab.frame_hover_link.clear();
    tab.has_caret = false;
    tab.frame_dirty = false;
    return;
  }
  const int width = tab.viewport_width > 0 ? tab.viewport_width : kDefaultRemoteViewportWidth;
  const int height = tab.viewport_height > 0 ? tab.viewport_height : kDefaultRemoteViewportHeight;
  // A page created after the viewport was reported (a navigation) still has to
  // learn the display's scale; setting it here covers every path and is cheap.
  tab.page->SetDeviceScaleFactor(tab.device_scale);
  // The viewport changed (or the page was never laid out): lay out here on the
  // worker thread.  SetTabViewport already lays out for a resize; this covers
  // the first frame after a load and any residual mismatch — including a
  // document whose on-demand geometry layout ran with a zero viewport height
  // (page scripts query geometry before the first frame; a zero-height layout
  // would otherwise stick for the document's whole life, zeroing vh units /
  // window.innerHeight and the IntersectionObserver band that lazy loading
  // hangs off).
  if (!tab.page->HasLayout() || tab.page->viewport_css_height() <= 0.0F) {
    tab.page->Layout(static_cast<float>(width), static_cast<float>(height));
  }
  const float content_height = tab.page->ContentHeight();
  paint::Rasterizer raster(width, height);
  // Rasterize in parallel bands on the shared pool: frame production is the
  // most latency-sensitive CPU work on this thread.  RasterizeParallel runs
  // the first band on this thread and keeps the submitted task count below the
  // worker count, so a pool task can still be waiting on the DOM lock this
  // call holds without deadlocking (threading-model §3.3).
  tab.page->RasterizeFull(raster, tab.scroll_offset_y, raster_pool_.get());
  auto frame = std::make_shared<RemoteFrame>();
  frame->width = width;
  frame->height = height;
  const std::vector<std::uint8_t>& pixels = raster.pixels();
  frame->rgba.assign(pixels.begin(), pixels.end());
  // Hyperlink under the pointer, computed here so the GUI can show the
  // pointing hand without the DOM.  Re-resolve the hovered element first: a
  // script may have removed the node recorded earlier, leaving a dangling
  // pointer (this crashed in HyperlinkTarget when it walked a freed node's
  // ancestors).  The layout was just rebuilt, so ElementAt is safe.
  ResolveHover(tab);
  std::string hover_link;
  if (tab.hovered_element != nullptr) {
    if (std::optional<std::string> target = HyperlinkTarget(tab.hovered_element, tab.url);
        target.has_value()) {
      hover_link = std::move(target.value());
    }
  }
  // Caret overlay geometry (device px, document coordinates).
  bool has_caret = false;
  float caret_x = 0;
  float caret_y = 0;
  float caret_height = 0;
  if (const auto geometry = tab.page->FocusedCaretGeometry(); geometry.has_value()) {
    has_caret = true;
    caret_x = geometry->x;
    caret_y = geometry->y;
    caret_height = geometry->height;
  }
  const std::uint64_t layout_version = tab.page->layout_version();
  // Publish under the controller mutex: the GUI copies these through
  // TabSnapshot on its own thread while the worker keeps rendering.
  std::lock_guard<std::mutex> lock(mutex_);
  tab.frame = std::move(frame);
  tab.frame_content_height = content_height;
  tab.frame_hover_link = std::move(hover_link);
  tab.has_caret = has_caret;
  tab.caret_x = caret_x;
  tab.caret_y = caret_y;
  tab.caret_height = caret_height;
  tab.frame_layout_version = layout_version;
  tab.frame_dirty = false;
}

void BrowserController::RefreshTabTitle(Tab& tab)
{
  if (tab.page == nullptr) {
    return;
  }
  const std::string title = tab.page->document()->Title();
  if (title.empty()) {
    return; // keep the URL fallback set at commit time
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (tab.title != title) {
    tab.title = title;
  }
}

// Reflects a script's style-relevant DOM mutations: the scoped restyle for
// attribute-only batches (layout follows only when a computed style actually
// changed), the full ApplyStyles path for conservative ones.  Returns whether
// any style-relevant mutation was pending (callers OR it with the DOM flag
// for the late-fetch pass).
bool BrowserController::ReflectScriptStyleMutations(Tab& tab)
{
  if (tab.script_runtime == nullptr || tab.page == nullptr) {
    return false;
  }
  const javascript::DomBinder::StyleDirtyScope scope = tab.script_runtime->TakeStyleDirtyScope();
  if (!scope.dirty) {
    return false;
  }
  // Handlers change document.title too (an oninput mirroring a field into the
  // title, SPA route titles): a title write is a DOM mutation and therefore
  // always part of a dirty batch.  The pump only re-reads the title when its
  // own timers ran scripts, so event dispatches must do it here.
  RefreshTabTitle(tab);
  if (scope.full_restyle || scope.roots.empty()) {
    tab.page->ReapplyStyles();
  } else {
    tab.page->ReapplyStylesFor(scope.roots);
  }
  return true;
}

int BrowserController::PumpScriptTimersUntilQuiet(int max_iterations)
{
  Tab* tab = ActiveTab();
  if (tab == nullptr) {
    return 0;
  }
  // Remote tabs: the child owns its own loop, and its Pump() reply reports
  // whether anything changed.
  if (IsRemoteTab(*tab)) {
    PumpScriptTimers();
    return 1;
  }
  if (tab->script_runtime == nullptr) {
    return 0;
  }
  // The timers run page scripts, so the pump must own the document (ADR
  // 0020): the pool threads that inject subresources (source collection,
  // image attach) take the same lock, and without it TSan reports the JS
  // attribute writes racing those locked reads.  The same lock then covers
  // the restyle and the queued load/error event callbacks below.
  // Keep the Page alive for as long as the lock is held (declared before
  // |dom_lock| so it is released last): a timer may navigate, which replaces
  // the tab's Page and would otherwise leave the lock guarding a destroyed
  // mutex.
  const std::shared_ptr<renderer::Page> page_lock_keepalive = tab->page;
  std::unique_lock<std::recursive_mutex> dom_lock;
  if (page_lock_keepalive != nullptr) {
    dom_lock = page_lock_keepalive->AcquireDomLock();
  }
  // Pick up what a finished pool-side late-image pass signalled (it must not
  // touch the Tab): repaint mark and follow-up pass.
  ApplyDeferredImageFetchSignals(*tab);
  // Measure the turns locally and merge once, so a GUI-thread snapshot can
  // never observe a half-written series (stats_mutex_, not mutex_).
  // Apply the harness's rAF budget, if one was set, to the runtime that is about
  // to pump (read under stats_mutex_ because the GUI thread owns the setter).
  {
    std::size_t limit = 0;
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      limit = raf_per_turn_limit_;
    }
    if (limit != 0) {
      tab->script_runtime->SetRafPerTurnLimit(limit);
    }
  }
  PumpStats pump_turns;
  const int iterations = ::neko::browser::PumpScriptTimersUntilQuiet(
      *tab->script_runtime, max_iterations, &pump_turns);
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    MergePump(pump_turn_stats_, pump_turns);
  }
  if (iterations > 0 && tab->page != nullptr) {
    // The pump ran scripts: document.title may have changed (this is how the
    // headless dump-dom path sees a title set by a deferred callback).
    RefreshTabTitle(*tab);
    // Timers may have mutated the DOM; re-run the cascade so the next
    // Layout/Rasterize reflects the new state, and pick up image sources the
    // callbacks assigned (lazy loading after the initial pass).  Batches that
    // only re-pointed media sources (img.src) skip the restyle.
    const bool style_changed = ReflectScriptStyleMutations(*tab);
    const bool dom_changed = tab->script_runtime->TakeDomDirty();
    if (style_changed || dom_changed) {
      SchedulePendingImageFetch(*tab);
    }
  }
  // Deliver any image load/error events the pool queued during the pump.
  // These dispatch page scripts, so they stay inside the DOM lock scope.
  FirePendingImageEvents(*tab);
  // A timer (or an image event handler) may have called form.submit() /
  // requestSubmit(); run the newest request now.  The outer keepalive keeps
  // the lock's Page alive across the navigation this may start.
  DrainPendingFormSubmission(*tab);
  return iterations;
}

void BrowserController::PumpScriptTimers()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr) {
    return;
  }
  // Keep the Page alive for as long as the lock is held.  Declared *before*
  // |dom_lock| on purpose: locals are destroyed in reverse declaration order, so
  // the keepalive must be released last, after the lock is dropped.
  //
  // Everything below can navigate -- a timer callback assigning location.href, a
  // script-requested navigation, a form submit -- and navigation replaces the
  // tab's Page, dropping the last reference to the current one.  Locking through
  // a borrowed `tab->page` and then unlocking after the Page had been destroyed
  // was a use-after-free on its mutex (caught by ThreadSanitizer; undefined
  // behaviour per the standard).
  //
  // The lock covers the whole pump, image event dispatch and IntersectionObserver
  // refresh included: all of it runs page scripts and mutates the document,
  // and the pool's subresource tasks take the same lock (ADR 0020).
  const std::shared_ptr<renderer::Page> page_lock_keepalive = tab->page;
  std::unique_lock<std::recursive_mutex> dom_lock;
  if (page_lock_keepalive != nullptr) {
    dom_lock = page_lock_keepalive->AcquireDomLock();
  }
  // Pick up what a finished pool-side late-image pass signalled (it must not
  // touch the Tab): repaint mark and follow-up pass.
  ApplyDeferredImageFetchSignals(*tab);
  // Images attached by the pool since the last pump fire their load/error
  // events first: page scripts often hang state (placeholder removal,
  // fallback swaps) off them, and any DOM they touch must land before the
  // frame is produced below.  The IntersectionObserver refresh re-evaluates
  // observed targets as content arrives and the page scrolls.  Both are
  // gated so a pump with no observers, no queued events and no timers stays a
  // no-op (the leftover DOM-dirty flag from an earlier operation must not be
  // turned into a version bump by this ambient check).
  const std::size_t image_events = FirePendingImageEvents(*tab);
  bool io_active = false;
  if (tab->script_runtime != nullptr && tab->page != nullptr) {
    io_active = tab->script_runtime->RefreshIntersectionObservers();
  }
  if ((image_events > 0 || io_active) && tab->script_runtime != nullptr && tab->page != nullptr) {
    const bool style_changed = ReflectScriptStyleMutations(*tab);
    const bool dom_changed = tab->script_runtime->TakeDomDirty();
    if (style_changed || dom_changed) {
      SchedulePendingImageFetch(*tab);
    }
  }
  if (IsRemoteTab(*tab)) {
    // Renderer mode: the child advances its own timers/animations; a changed
    // reply repaints, and a scroll performed since the last frame is
    // coalesced into this pump.
    auto reply = tab->session->Pump();
    if (!reply.has_value()) {
      MarkSessionFailed(*tab, reply.error().message());
      return;
    }
    ApplyRendererUpdate(*tab, reply.value());
    if (tab->remote_scroll_dirty) {
      PullRemoteFrame(*tab, /*force=*/false);
    }
    return;
  }
  if (tab->script_runtime != nullptr && tab->page != nullptr) {
    // This is the GUI's pump path (it calls RunPendingTimers directly rather than
    // the browser-layer PumpScriptTimersUntilQuiet), so the rAF budget and the
    // pump measurement belong here too -- otherwise --raf-per-turn and
    // --perf-report would describe the CLI only.
    std::size_t raf_limit = 0;
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      raf_limit = raf_per_turn_limit_;
    }
    if (raf_limit != 0) {
      tab->script_runtime->SetRafPerTurnLimit(raf_limit);
    }
    javascript::DomBinder::TurnBreakdown turn;
    const auto turn_start = std::chrono::steady_clock::now();
    const int pumped = tab->script_runtime->RunPendingTimers(&turn);
    {
      PumpStats turn_stats;
      AddPumpTurn(turn_stats,
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - turn_start)
                      .count(),
                  turn);
      std::lock_guard<std::mutex> lock(stats_mutex_);
      MergePump(pump_turn_stats_, turn_stats);
    }
    if (pumped > 0) {
      // The pump ran scripts: document.title follows them (async XHR
      // handlers, timers), like the tab strip in a real browser.
      RefreshTabTitle(*tab);
      // Timers may have mutated the DOM; re-run the cascade so the next
      // Layout/Rasterize reflects the new state, and fetch the image sources
      // assigned by lazy-loading callbacks (a claimed pass: steady state is one
      // DOM walk, no requests).  A batch that touched nothing (a pure rAF tick
      // or network poll) or only re-pointed media sources skips the restyle:
      // the full cascade used to run on every 50 ms pump (>600 ms per pass on
      // bilibili).
      const bool style_changed = ReflectScriptStyleMutations(*tab);
      const bool dom_changed = tab->script_runtime->TakeDomDirty();
      if (style_changed || dom_changed) {
        SchedulePendingImageFetch(*tab);
      }
    }
  }
  // Animated images (GIF) advance on the same frame clock; a changed frame
  // bumps the page version so the UI repaints.
  if (tab->page != nullptr) {
    (void)tab->page->AdvanceAnimations();
  }
  // A directly navigated animated GIF plays on the same clock.  Frames are
  // swapped by replacing the shared Image (the GUI re-reads the snapshot on
  // every pump) and image_frame lets it tell a repaint is due.
  if (tab->gif_animation != nullptr && tab->content_type == ContentType::kImage) {
    const double now_ms = static_cast<double>(NowMillis());
    if (!tab->gif_started) {
      // Defensive: an animation without a published start time begins now.
      tab->gif_started = true;
      tab->gif_start_ms = now_ms;
    }
    const image::GifAnimation& animation = *tab->gif_animation;
    if (image::AdvanceGifFrame(animation,
                               now_ms - tab->gif_start_ms,
                               tab->gif_frame,
                               tab->gif_loops,
                               tab->gif_finished) &&
        tab->gif_frame < animation.frames.size()) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto next = std::make_shared<image::Image>();
      next->width = animation.width;
      next->height = animation.height;
      next->rgba = animation.frames[tab->gif_frame].rgba;
      tab->image = std::move(next);
      ++tab->image_frame;
    }
  }
  // A timer or event handler may have called form.submit()/requestSubmit():
  // run the newest request.  Do this before window.location handling so a
  // submission is not dropped by a navigation the same callback requested
  // (last navigation still wins; both go through the same path).
  DrainPendingFormSubmission(*tab);
  // A timer callback may have requested a navigation (window.location): the
  // request was written into the tab by the runtime's callback, so act on it
  // now (and clear it so it fires once).
  const std::string requested_url = tab->pending_script_navigation.url;
  const bool requested_reload = tab->pending_script_navigation.is_reload;
  if (!requested_url.empty() || requested_reload) {
    tab->pending_script_navigation = {};
    const std::string target = requested_url.empty() ? tab->url : requested_url;
    if (!target.empty()) {
      NavigateToUrl(*tab, target);
    }
  }
  // Rebuild the viewport frame when the page changed or the GUI moved the
  // viewport / scroll bar since the last one (ADR 0020).  The GUI only draws
  // this frame; it never rasterizes or walks the DOM.
  if (tab->content_type == ContentType::kHtml) {
    if (tab->page == nullptr) {
      tab->frame = nullptr;
      tab->frame_content_height = 0;
    } else if (tab->frame_dirty || tab->frame_layout_version != tab->page->layout_version()) {
      ProduceFrame(*tab);
    }
  }
}
