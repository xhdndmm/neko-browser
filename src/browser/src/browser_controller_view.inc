void BrowserController::SetTabScrollOffset(int tab_id, float y)
{
  Tab* tab = nullptr;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& candidate : tabs_) {
      if (candidate->id == tab_id) {
        changed = candidate->scroll_offset_y != y;
        candidate->scroll_offset_y = y;
        candidate->frame_dirty = true;
        tab = candidate.get();
        break;
      }
    }
  }
  if (tab == nullptr) {
    return;
  }
  if (!IsRemoteTab(*tab)) {
    // In-process: the DOM is here, so the new scroll position is what the
    // page observes.  A changed offset fires the document-level "scroll"
    // event first (WHATWG HTML "scroll" for the viewport: the event target is
    // the Document, which is also where window-level listeners registered in
    // this engine); infinite-scroll feeds hang their next-page loads off it.
    // Then re-evaluate IntersectionObserver wiring (the other lazy-loading
    // signal on real pages) and pick up the image sources the resulting
    // callbacks assigned.
    if (tab->script_runtime != nullptr && tab->page != nullptr) {
      if (changed) {
        tab->script_runtime->DispatchDocumentEvent("scroll");
      }
      tab->script_runtime->RefreshIntersectionObservers();
      const bool style_changed = ReflectScriptStyleMutations(*tab);
      const bool dom_changed = tab->script_runtime->TakeDomDirty();
      if (style_changed || dom_changed) {
        SchedulePendingImageFetch(*tab);
      }
    }
    return;
  }
  // Renderer mode: keep the child's viewport offset (window.scrollY) in sync
  // and repaint at the new offset (throttled; the pump flushes coalesced
  // scrolls).
  auto reply = tab->session->ScrollTo(y);
  if (!reply.has_value()) {
    MarkSessionFailed(*tab, reply.error().message());
    return;
  }
  ApplyRendererUpdate(*tab, reply.value());
  PullRemoteFrame(*tab, /*force=*/false);
}

void BrowserController::SetTabViewport(int tab_id, int width, int height, float device_scale)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return;
  }
  width = std::max(1, width);
  height = std::max(1, height);
  if (tab->viewport_width == width && tab->viewport_height == height &&
      tab->device_scale == device_scale) {
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
  const std::shared_ptr<renderer::Page> page_lock_keepalive = tab->page;
  std::unique_lock<std::recursive_mutex> dom_lock;
  if (page_lock_keepalive != nullptr) {
    dom_lock = page_lock_keepalive->AcquireDomLock();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab->viewport_width = width;
    tab->viewport_height = height;
    tab->device_scale = device_scale;
    tab->frame_dirty = true;
  }
  if (tab->page != nullptr) {
    tab->page->SetDeviceScaleFactor(device_scale);
  }
  if (IsRemoteTab(*tab)) {
    // The page is laid out for the new viewport inside the child (kSnapshot
    // reports the size); the frame it returns is sized to match.
    PullRemoteFrame(*tab, /*force=*/true);
    return;
  }
  // In-process pages: the load path laid the page out at the controller's
  // default, so even the first report re-lays out and fires `resize` (the page
  // already observed the default size in its first script run — the same thing
  // a browser does when a window opens at its final size).  A re-report of the
  // same size never reaches this point (the early return above).
  if (tab->page != nullptr) {
    tab->page->Layout(static_cast<float>(width), static_cast<float>(height));
  }
  if (tab->script_runtime == nullptr) {
    return;
  }
  tab->script_runtime->DispatchDocumentEvent("resize");
  tab->script_runtime->NotifyMediaChanged();
}

float NextZoomFactor(float current, int direction)
{
  static constexpr float kLadder[] = {0.25F,
                                      0.33F,
                                      0.5F,
                                      0.67F,
                                      0.75F,
                                      0.8F,
                                      0.9F,
                                      1.0F,
                                      1.1F,
                                      1.25F,
                                      1.5F,
                                      1.75F,
                                      2.0F,
                                      2.5F,
                                      3.0F,
                                      4.0F,
                                      5.0F};
  if (direction == 0) {
    return current;
  }
  if (direction > 0) {
    for (const float step : kLadder) {
      if (step > current + 1e-4F) {
        return step;
      }
    }
    return current;
  }
  for (std::size_t i = sizeof(kLadder) / sizeof(kLadder[0]); i > 0; --i) {
    if (kLadder[i - 1] < current - 1e-4F) {
      return kLadder[i - 1];
    }
  }
  return current;
}

float BrowserController::SetTabZoom(int tab_id, float factor)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return 1.0F;
  }
  const float applied = std::clamp(factor, renderer::kMinUserZoom, renderer::kMaxUserZoom);
  if (tab->zoom == applied) {
    return applied;
  }
  if (IsRemoteTab(*tab)) {
    // The child owns the CSS-pixel mapping in this mode; it re-lays out and the
    // next snapshot comes back at the new scale.
    auto reply = tab->session->SetZoom(applied);
    if (!reply.has_value()) {
      // A dead session is replaced by the regular crash-recovery path; the
      // zoom value still sticks so the replacement renders zoomed.
      MarkSessionFailed(*tab, reply.error().message());
    }
  } else if (tab->page != nullptr) {
    tab->page->SetUserZoom(applied);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab->zoom = applied;
  }
  if (IsRemoteTab(*tab)) {
    // Force the frame now: the GUI repaints off the new snapshot.
    PullRemoteFrame(*tab, /*force=*/true);
  }
  return applied;
}

float BrowserController::ZoomInTab(int tab_id)
{
  return SetTabZoom(tab_id, NextZoomFactor(TabZoom(tab_id), 1));
}

float BrowserController::ZoomOutTab(int tab_id)
{
  return SetTabZoom(tab_id, NextZoomFactor(TabZoom(tab_id), -1));
}

float BrowserController::ResetTabZoom(int tab_id)
{
  return SetTabZoom(tab_id, 1.0F);
}

float BrowserController::TabZoom(int tab_id) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& tab : tabs_) {
    if (tab->id == tab_id) {
      return tab->zoom;
    }
  }
  return 1.0F;
}

int BrowserController::FindInTab(int tab_id, std::string_view query, int direction)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return 0;
  }
  if (query.empty()) {
    ClearFindInTab(tab_id);
    return 0;
  }
  const int viewport_height =
      tab->viewport_height > 0 ? tab->viewport_height : kDefaultRemoteViewportHeight;
  renderer::FindMatch match;
  int count = 0;
  int index = -1;
  if (IsRemoteTab(*tab)) {
    // The child owns the match list; it answers with the count and the current
    // rectangle, so the browser only has to scroll to it.
    auto reply = tab->session->Find(query, direction);
    if (!reply.has_value()) {
      MarkSessionFailed(*tab, reply.error().message());
      return 0;
    }
    count = reply.value().find_count;
    index = reply.value().find_index;
    match = renderer::FindMatch{reply.value().find_x,
                                reply.value().find_y,
                                reply.value().find_width,
                                reply.value().find_height};
    std::lock_guard<std::mutex> lock(mutex_);
    tab->find_query = std::string(query);
    tab->find_index = index;
    tab->find_match = match;
    tab->find_match_count = count;
  } else {
    if (tab->page == nullptr) {
      return 0;
    }
    // Lock order: the Page lock is always taken *before* the controller mutex.
    // Page::FindMatches takes the page lock internally, so it must run with
    // mutex_ released -- calling it under mutex_ (as this function used to)
    // inverted the order against ProduceFrame, which lays the page out and only
    // then takes mutex_ to publish the frame.  That is an ABBA deadlock, and the
    // comment here used to assert the opposite (wrong) invariant.
    bool recompute = false;
    {
      std::lock_guard<std::mutex> probe(mutex_);
      recompute = direction == 0 || tab->find_query != query;
    }
    std::vector<renderer::FindMatch> matches;
    if (recompute) {
      matches = tab->page->FindMatches(query);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (recompute) {
      tab->find_query = std::string(query);
      tab->find_matches = std::move(matches);
      tab->find_index = tab->find_matches.empty() ? -1 : 0;
    } else if (!tab->find_matches.empty()) {
      const int size = static_cast<int>(tab->find_matches.size());
      tab->find_index = ((tab->find_index + direction) % size + size) % size;
    }
    count = static_cast<int>(tab->find_matches.size());
    index = tab->find_index;
    if (index >= 0 && index < count) {
      match = tab->find_matches[static_cast<std::size_t>(index)];
    }
    tab->find_match = match;
    tab->find_match_count = count;
  }
  if (index >= 0 && count > 0) {
    // Centre the match in the viewport, like browsers do, by requesting a
    // scroll (the GUI applies it to its scroll bar through the latch).
    const float centre = match.y + match.height / 2.0F;
    SetTabScrollRequest(tab_id,
                        std::max(0.0F, centre - static_cast<float>(viewport_height) / 3.0F));
  } else if (IsRemoteTab(*tab)) {
    // "No matches" must repaint too: pull a frame so the GUI clears the
    // previous highlight state.
    PullRemoteFrame(*tab, /*force=*/true);
  }
  return count;
}

void BrowserController::ClearFindInTab(int tab_id)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab->find_query.clear();
    tab->find_matches.clear();
    tab->find_index = -1;
    tab->find_match = renderer::FindMatch{};
    tab->find_match_count = 0;
  }
  if (IsRemoteTab(*tab)) {
    // Clear the child's list too, otherwise its next step would answer from a
    // stale query.
    auto reply = tab->session->Find("", 0);
    if (!reply.has_value()) {
      MarkSessionFailed(*tab, reply.error().message());
    }
  }
}

void BrowserController::SetTabScrollRequest(int tab_id, float y)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& tab : tabs_) {
    if (tab->id == tab_id) {
      tab->pending_scroll_y = y;
      ++tab->scroll_request_id;
      tab->frame_dirty = true;
      return;
    }
  }
}
