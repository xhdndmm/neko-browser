// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

std::string BrowserController::ResolveInput(const std::string& input) const
{
  std::string s = std::string(Trim(input));
  if (s.empty())
    return {};
#ifdef _WIN32
  // Drive-letter and UNC paths are filesystem paths, not URLs: "C:\dir\a.html"
  // parses as the one-letter scheme "c", and the http://-prefixing rule below
  // would turn it into a bogus URL.  Hand it through unchanged; NavigateToUrl
  // routes it to the filesystem.
  if (IsWindowsLocalPath(s)) {
    return s;
  }
#endif
  // Absolute URL already?
  auto parsed = url::Url::Parse(s);
  if (parsed.has_value())
    return s;
  // Bare hostname like "example.com" -> http://example.com
  if (s.find(' ') == std::string::npos && s.find('/') == std::string::npos &&
      s.find('.') != std::string::npos) {
    return "http://" + s;
  }
  // Anything with whitespace, or a single word without a dot or path
  // separator, is a search query ("weather", "hello world"): resolve it
  // through the configured search engine (Settings > Default search engine).
  const bool looks_like_path =
      s.find('/') != std::string::npos || s.find('\\') != std::string::npos;
  if (s.find(' ') != std::string::npos || (!looks_like_path && s.find('.') == std::string::npos)) {
    return SearchUrlFor(s);
  }
  // Treat as a local path.
  return s;
}

std::string BrowserController::SearchUrlFor(std::string_view query) const
{
  const std::string custom = GetPreference(prefs::kSearchEngineTemplate);
  if (!custom.empty()) {
    return BuildSearchUrl(custom, query);
  }
  const std::string id = GetPreference(prefs::kSearchEngine);
  return BuildSearchUrl(DefaultSearchEngine(id).url_template, query);
}

void BrowserController::NavigateToUrl(Tab& tab,
                                      const std::string& url_string,
                                      std::string method,
                                      std::string body,
                                      std::string content_type)
{
  // Every exit path below replaces the tab's document (or its error page);
  // release the previous document's arena pages when the navigation is done.
  // See base::ReleaseFreeMemory and HeapTrimOnExit.
  [[maybe_unused]] const HeapTrimOnExit trim_heap_on_exit;

  // Record the requested URL up front (under the controller mutex, like every
  // other Tab field write).  A navigation triggered from inside LoadBytes
  // (a page script assigning window.location) runs synchronously through this
  // function again; tracking the URL here — instead of in the callers — keeps
  // the final address bar correct no matter how deep the chain goes.
  // The document that initiated this navigation is the current one: its site
  // decides the SameSite filter for the navigation request (RFC 6265bis
  // §5.4.2).  Captured before |tab.url| moves to the new document below.
  std::string navigation_initiator_site;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const base::Result<url::Url> previous = url::Url::Parse(tab.url); previous.has_value()) {
      navigation_initiator_site = security::SiteForCookies(previous.value());
    }
    tab.url = url_string;
  }
  // A fresh document replaces the old one; any focused element is stale.
  tab.focused_element = nullptr;
  tab.autofocus_processed.clear();
  // A form submission requested against the old document is stale too.
  tab.pending_form_submission = {};
#ifdef _WIN32
  // Windows local paths are also valid URLs: "C:\dir\page.html" parses as the
  // one-letter scheme "c".  They belong to the filesystem, so route them before
  // the URL parse below (the address bar, bookmarks and the tests all feed
  // plain drive paths here).
  if (IsWindowsLocalPath(url_string)) {
    LoadLocalPath(tab, url_string);
    return;
  }
#endif
  // Local paths (and file:// URLs) are handled directly without the URL
  // parser (which does not support opaque file: URLs yet).
  if (base::AsciiStartsWith(url_string, "file://")) {
    // "file://<host>/<path>": only the empty-host form ("file:///path", the
    // form links and bookmarks produce) is supported, and the path is absolute
    // — stripping the leading slash here used to turn it into a relative path
    // that never resolved.
    LoadLocalPath(tab, url_string.substr(7));
    return;
  }
  auto parsed = url::Url::Parse(url_string);
  if (!parsed.has_value()) {
    // Not a URL at all: treat it as a local path.
    LoadLocalPath(tab, url_string);
    return;
  }
  if (parsed.value().scheme() == "file") {
    LoadLocalPath(tab, parsed.value().path());
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.loading = true;
  }
  FetchAndLoad(tab,
               parsed.value(),
               navigation_initiator_site,
               std::move(method),
               std::move(body),
               std::move(content_type));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.loading = false;
  }
}

void BrowserController::LoadLocalPath(Tab& tab, const std::string& path)
{
  // Strip any query/fragment from a self-referential form submission (e.g.
  // form.html?q=hello): the file on disk is form.html; the query stays in
  // tab.url so window.location.search reflects it.
  std::string file = path;
#ifdef _WIN32
  // "file:///C:/dir/page.html" contributes a leading slash before the drive
  // letter ("/C:/dir/page.html"); fopen() does not resolve that form.
  if (file.size() >= 3 && (file[0] == '/' || file[0] == '\\') && file[2] == ':' &&
      std::isalpha(static_cast<unsigned char>(file[1])) != 0) {
    file.erase(0, 1);
  }
#endif
  const std::size_t q = file.find_first_of("?#");
  if (q != std::string::npos) {
    file.resize(q);
  }
  auto maybe_bytes = storage::ReadFile(file);
  if (!maybe_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.content_type = ContentType::kError;
    tab.error = std::make_shared<std::string>("cannot read file: " + maybe_bytes.error().message());
    tab.title = "File error";
    return;
  }
  // base_url keeps the full reference (query included) so window.location
  // reflects a self-submitting form's query string.
  LoadBytes(tab, maybe_bytes.value(), "", "file://" + path);
}

base::Result<void> BrowserController::Navigate(int tab_id, const std::string& input)
{
  const std::string target = ResolveInput(input);
  if (target.empty())
    return base::Err(base::Error::InvalidArgument("empty URL"));
  return NavigateWithRequestBody(tab_id, target, "GET", {}, {});
}

base::Result<void> BrowserController::NavigateWithRequestBody(int tab_id,
                                                              const std::string& target,
                                                              std::string method,
                                                              std::string body,
                                                              std::string content_type)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr)
    return base::Err(base::Error::InvalidArgument("no such tab"));

  // Push onto the back/forward stack (truncating any forward entries).
  // The history stack is worker-thread only, so it needs no lock.
  if (!tab->history.empty() && tab->history_index >= 0 &&
      tab->history[static_cast<size_t>(tab->history_index)] == target) {
    // Reload of the current entry.
  } else {
    if (tab->history_index >= 0 && tab->history_index + 1 < static_cast<int>(tab->history.size())) {
      tab->history.resize(static_cast<size_t>(tab->history_index) + 1);
    }
    tab->history.push_back(target);
    tab->history_index = static_cast<int>(tab->history.size()) - 1;
  }

  NavigateToUrl(*tab, target, std::move(method), std::move(body), std::move(content_type));

  return base::Ok();
}

bool BrowserController::DispatchPointerClick(int tab_id, float doc_x, float doc_y)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return false;
  }
  // Hold the DOM lock while the click's scripts run (ADR 0020): the pool
  // threads that inject subresources take the same lock.
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
  tab->frame_dirty = true; // focus / :active may change the caret
  // Renderer-process mode: the child owns the DOM, so the click is forwarded
  // and it runs the same dispatch code (returns whether the default action
  // ran).
  if (IsRemoteTab(*tab)) {
    auto reply = tab->session->Click(doc_x, doc_y);
    if (!reply.has_value()) {
      MarkSessionFailed(*tab, reply.error().message());
      return false;
    }
    const bool handled = reply.value().handled;
    ApplyRendererUpdate(*tab, reply.value());
    return handled;
  }
  if (tab->content_type != ContentType::kHtml || tab->page == nullptr) {
    return false;
  }
  const dom::Element* element = tab->page->ElementAt(doc_x, doc_y);
  if (element == nullptr) {
    return false;
  }
  // Inert subtrees are not clickable (HTML §6.6.1): the pointer events below
  // still dispatch (documented approximation -- a conforming inert subtree
  // would swallow them), but the element does not take focus and its default
  // action (toggle / navigation) does not run.
  const bool inert = InInertSubtree(*element);
  // Focus the clicked element when it is a focusable area (a form control, a
  // link, or anything with a tabindex attribute); clicking anywhere else
  // clears the focus.  The page keeps the focused element for the UI's caret
  // painting (read across threads under the page lock).  Focus changes run the
  // blur/focusout/focus/focusin sequence.
  if (!inert) {
    dom::Element* focus_target = const_cast<dom::Element*>(element);
    if (!IsFocusable(*focus_target)) {
      // A click on the non-focusable content of a disclosure control focuses
      // the summary itself (HTML §4.11.1 makes the summary a focusable area).
      dom::Element* summary = NearestSummary(*focus_target);
      if (summary != nullptr && IsFocusable(*summary)) {
        focus_target = summary;
      }
    }
    if (IsFocusable(*focus_target)) {
      // Pointer focus leaves a caret at the end rather than selecting the
      // whole value (click-position hit testing is not modelled; keyboard and
      // script focus do select the contents).
      FocusControl(tab, focus_target, nullptr, nullptr, /*select_contents=*/false);
    } else if (tab->focused_element != nullptr) {
      FocusControl(tab, nullptr);
    }
  }
  // Run the page's cancelable pointer events (mousedown -> mouseup -> click)
  // when a script runtime exists; a page with no scripts skips straight to the
  // default action.  The document owns the element, so const_cast is safe.
  if (tab->script_runtime != nullptr) {
    dom::Element& el = const_cast<dom::Element&>(*element);
    (void)tab->script_runtime->DispatchMouseEvent(
        el, "mousedown", static_cast<double>(doc_x), static_cast<double>(doc_y), 0);
    (void)tab->script_runtime->DispatchMouseEvent(
        el, "mouseup", static_cast<double>(doc_x), static_cast<double>(doc_y), 0);
    const bool not_canceled = tab->script_runtime->DispatchMouseEvent(
        el, "click", static_cast<double>(doc_x), static_cast<double>(doc_y), 0);
    // A pointer handler may have mutated the DOM; reflect it before the
    // default action (which may navigate away).  Media-source mutations (a
    // lazy-load handler assigning img.src) only need the late-fetch pass.
    const bool style_changed = ReflectScriptStyleMutations(*tab);
    const bool dom_changed = tab->script_runtime->TakeDomDirty();
    if (style_changed || dom_changed) {
      SchedulePendingImageFetch(*tab);
    }
    // A pointer handler may have submitted the form through script
    // (form.submit()/requestSubmit()); that request takes over the default
    // action (the spec's planned navigation, last one wins).
    if (tab->pending_form_submission.form != nullptr) {
      DrainPendingFormSubmission(*tab);
      return true;
    }
    if (!not_canceled) {
      return true; // preventDefault: the page handled the click.
    }
  }
  if (inert) {
    // An inert element has no activation behaviour: no toggle, no navigation,
    // no form submission.  Events above already dispatched (approximation).
    return false;
  }
  // Default action: a checkable input toggles its checkedness (HTML
  // §4.10.5.3).  The toggle changes the cascade (input:checked) and the
  // widget paint state, so re-run styles; the frame pump rebuilds the rest.
  if (ToggleCheckableInput(const_cast<dom::Element&>(*element))) {
    dom::Element& el = const_cast<dom::Element&>(*element);
    DispatchToggleEvents(tab->script_runtime.get(), el);
    tab->page->ReapplyStyles();
    return true;
  }
  // Default action: navigate an <a href> hyperlink (or a clickable element
  // nested inside one).
  const std::optional<std::string> target = HyperlinkTarget(element, tab->url);
  if (target.has_value()) {
    static_cast<void>(Navigate(tab_id, target.value()));
    return true;
  }
  // Default action: a submit button submits its form (the clicked button is
  // the submitter: its name/value and form*method/enctype/target overrides).
  if (dom::Element* submitter = FindSubmitButton(const_cast<dom::Element*>(element))) {
    if (dom::Element* form = html::OwnerForm(*submitter)) {
      SubmitForm(tab_id, form, submitter);
      return true;
    }
  }
  // Default action: clicking the disclosure control (the first <summary>
  // child) toggles its <details> and fires the non-bubbling "toggle" event
  // (HTML §4.11.1).  A canceled click returned above, so preventDefault keeps
  // the widget closed/open.
  if (dom::Element* details = DetailsForClick(*element)) {
    ToggleDetails(*details);
    // Re-lay out before the toggle listener runs so it observes the opened or
    // collapsed box tree (the spec's task is queued after the rendering update
    // opportunity; this is the documented synchronous approximation).
    tab->page->ReapplyStyles();
    if (tab->script_runtime != nullptr) {
      tab->script_runtime->DispatchNonBubblingEvent(*details, "toggle");
    }
    return true;
  }
  return false;
}

void BrowserController::DispatchHover(int tab_id, float doc_x, float doc_y)
{
  Tab* tab = FindTab(tab_id);
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
  const std::shared_ptr<renderer::Page> page_lock_keepalive = tab->page;
  std::unique_lock<std::recursive_mutex> dom_lock;
  if (page_lock_keepalive != nullptr) {
    dom_lock = page_lock_keepalive->AcquireDomLock();
  }
  if (IsRemoteTab(*tab)) {
    tab->frame_dirty = true; // the hover link drives the cursor
    auto reply = tab->session->Hover(doc_x, doc_y);
    if (!reply.has_value()) {
      MarkSessionFailed(*tab, reply.error().message());
      return;
    }
    ApplyRendererUpdate(*tab, reply.value());
    return;
  }
  if (tab->content_type != ContentType::kHtml || tab->page == nullptr) {
    return;
  }
  // Re-resolve the previous hover from the stored position: a page script may
  // have removed that node since the last frame, leaving a dangling pointer
  // (mouseout would then dereference freed memory).
  ResolveHover(*tab);
  dom::Element* prev = tab->hovered_element;
  const dom::Element* element = tab->page->ElementAt(doc_x, doc_y);
  tab->hover_x = doc_x;
  tab->hover_y = doc_y;
  tab->has_hover = true;
  if (element == prev) {
    return;
  }
  if (prev != nullptr && tab->script_runtime != nullptr) {
    tab->script_runtime->DispatchMouseEvent(
        *prev, "mouseout", static_cast<double>(doc_x), static_cast<double>(doc_y), 0);
  }
  tab->hovered_element = const_cast<dom::Element*>(element);
  // Drive the style engine's :hover.  SetHoveredElement reapplies the cascade
  // (so hover-only rules such as ".dropdown:hover .dropdown-content{
  // display:block}" take effect) and bumps the page version, which makes the
  // worker rebuild the frame — the GUI never touches the DOM.
  tab->page->SetHoveredElement(element);
  if (element != nullptr && tab->script_runtime != nullptr) {
    tab->script_runtime->DispatchMouseEvent(*const_cast<dom::Element*>(element),
                                            "mouseover",
                                            static_cast<double>(doc_x),
                                            static_cast<double>(doc_y),
                                            0);
  }
  // A hover handler may mutate the DOM; reflect it.
  if (tab->script_runtime != nullptr) {
    const bool style_changed = ReflectScriptStyleMutations(*tab);
    const bool dom_changed = tab->script_runtime->TakeDomDirty();
    if (style_changed || dom_changed) {
      SchedulePendingImageFetch(*tab);
    }
  }
}

void BrowserController::DispatchHoverClear(int tab_id)
{
  Tab* tab = FindTab(tab_id);
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
  const std::shared_ptr<renderer::Page> page_lock_keepalive = tab->page;
  std::unique_lock<std::recursive_mutex> dom_lock;
  if (page_lock_keepalive != nullptr) {
    dom_lock = page_lock_keepalive->AcquireDomLock();
  }
  tab->frame_dirty = true;
  if (IsRemoteTab(*tab)) {
    auto reply = tab->session->HoverClear();
    if (!reply.has_value()) {
      MarkSessionFailed(*tab, reply.error().message());
      return;
    }
    ApplyRendererUpdate(*tab, reply.value());
    return;
  }
  // Re-resolve before the mouseout dispatch (the node may have been removed by
  // a script), then drop the hover entirely.
  ResolveHover(*tab);
  tab->has_hover = false;
  if (tab->hovered_element != nullptr) {
    if (tab->script_runtime != nullptr) {
      tab->script_runtime->DispatchMouseEvent(*tab->hovered_element, "mouseout", 0, 0, 0);
    }
    tab->hovered_element = nullptr;
  }
  // Drop the style engine's :hover so hover-only rules collapse again.
  if (tab->page != nullptr) {
    tab->page->SetHoveredElement(nullptr);
  }
}

bool BrowserController::DispatchWheel(int tab_id, double delta_y)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return false;
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
  tab->frame_dirty = true;
  if (IsRemoteTab(*tab)) {
    auto reply = tab->session->Wheel(delta_y);
    if (!reply.has_value()) {
      MarkSessionFailed(*tab, reply.error().message());
      return false;
    }
    ApplyRendererUpdate(*tab, reply.value());
    return true;
  }
  if (tab->content_type != ContentType::kHtml || tab->page == nullptr) {
    return false;
  }
  dom::Element* target = tab->focused_element;
  if (target == nullptr) {
    if (tab->page->document() != nullptr) {
      target = dom::QuerySelector(*tab->page->document(), "body");
    }
  }
  if (target == nullptr || tab->script_runtime == nullptr) {
    return true;
  }
  const bool not_canceled = tab->script_runtime->DispatchWheelEvent(*target, "wheel", delta_y);
  // A wheel handler may mutate the DOM (e.g. lazy-load placeholders); reflect
  // it so the change appears without waiting for the next navigation.
  const bool wheel_style_changed = ReflectScriptStyleMutations(*tab);
  const bool wheel_dom_changed = tab->script_runtime->TakeDomDirty();
  if (wheel_style_changed || wheel_dom_changed) {
    SchedulePendingImageFetch(*tab);
  }
  return not_canceled;
}

bool BrowserController::DispatchKeyboard(int tab_id,
                                         std::string_view type,
                                         std::string_view key,
                                         std::string_view code)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return false;
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
  tab->frame_dirty = true; // a key edit moves the caret / mutates the DOM
  if (IsRemoteTab(*tab)) {
    auto reply = tab->session->Key(type, key, code);
    if (!reply.has_value()) {
      MarkSessionFailed(*tab, reply.error().message());
      return false;
    }
    const bool not_canceled = reply.value().handled;
    ApplyRendererUpdate(*tab, reply.value());
    return not_canceled;
  }
  if (tab->content_type != ContentType::kHtml || tab->page == nullptr) {
    return false;
  }
  // Keyboard events target the focused element; without focus, <body>.
  dom::Element* target = tab->focused_element;
  if (target == nullptr) {
    if (tab->page->document() != nullptr) {
      target = dom::QuerySelector(*tab->page->document(), "body");
    }
  }
  // Run the cancelable keydown, then the default action unless canceled.
  const bool not_canceled =
      tab->script_runtime != nullptr && target != nullptr
          ? tab->script_runtime->DispatchKeyboardEvent(*target, type, key, code)
          : true;
  // A key handler may have submitted a form through script; that navigation
  // takes over (nothing below may run against the replaced document).
  if (tab->pending_form_submission.form != nullptr) {
    const std::shared_ptr<renderer::Page> page_before = tab->page;
    DrainPendingFormSubmission(*tab);
    if (tab->page != page_before) {
      return not_canceled;
    }
  }
  if (!not_canceled || type != "keydown") {
    return not_canceled;
  }
  // Default actions: Tab / Backtab (Qt's Shift+Tab) move keyboard focus
  // through the document's focusable controls; Space toggles a focused
  // checkable input (keyboard activation); otherwise a printable character
  // inserts into a focused text input, Backspace deletes, Enter submits the
  // enclosing form (implicit submission).
  dom::Element* control = tab->focused_element;
  if (key == "Tab" || key == "Backtab") {
    FocusControl(tab, NextFocusable(tab, /*backwards=*/key == "Backtab"));
    return not_canceled;
  }
  if (control != nullptr && (key == " " || key == "Space" || key == "Spacebar") &&
      ToggleCheckableInput(*control)) {
    DispatchToggleEvents(tab->script_runtime.get(), *control);
    tab->page->ReapplyStyles();
    return not_canceled;
  }
  // Keyboard activation of a focused <summary> disclosure control: Enter and
  // Space toggle its <details> (HTML §4.11.1).  The focused element must be
  // the summary itself; interactive descendants (links/controls) have their
  // own key handling above/below.
  if (control != nullptr && control->tag_name() == "summary" &&
      (key == " " || key == "Space" || key == "Spacebar" || key == "Enter")) {
    if (dom::Element* details = DetailsOfSummary(*control)) {
      ToggleDetails(*details);
      tab->page->ReapplyStyles();
      if (tab->script_runtime != nullptr) {
        tab->script_runtime->DispatchNonBubblingEvent(*details, "toggle");
      }
      return not_canceled;
    }
  }
  const bool input_is_text = control != nullptr && control->tag_name() == "input";
  bool value_changed = false;
  if (control != nullptr && IsKeyboardEditableControl(*control)) {
    const bool is_input = input_is_text;
    // The keyboard and JavaScript layers share one value store and one
    // selection state (dom::Element::TextControlState).  A printable key
    // replaces the selected range (or inserts at the collapsed caret);
    // Backspace/Delete remove the selection or the adjacent code point.  Every
    // edit is stored through the shared per-type value sanitization algorithm
    // (html::InputValue / html::SetInputValue).  Rejected user text is not
    // retained (documented badInput limitation), so an edit whose sanitized
    // result differs can also shrink the value.
    std::string value = html::InputValue(*control);
    std::size_t length = dom::Utf16Length(value);
    dom::TextControlState& state = control->EnsureTextControlState();
    state.selection_start = std::min(state.selection_start, length);
    state.selection_end = std::min(state.selection_end, length);

    const auto store_value = [&](const std::string& updated) {
      const std::string previous = html::InputValue(*control);
      if (is_input) {
        html::SetInputValue(*control, updated);
      } else {
        while (control->first_child() != nullptr) {
          control->RemoveChild(control->first_child());
        }
        control->AppendChild(std::make_unique<dom::Text>(updated));
      }
      state.dirty_value = true;
      // Compare the sanitized values: typing a character a state's sanitizer
      // rejects (e.g. "x" into a number control) must not fire `input`.
      if (html::InputValue(*control) != previous) {
        value_changed = true;
      }
    };
    const auto replace_selection = [&](std::string_view inserted) {
      const std::size_t caret = state.selection_start + dom::Utf16Length(inserted);
      value = dom::ReplaceUtf16Range(value, state.selection_start, state.selection_end, inserted);
      store_value(value);
      state.selection_start = caret;
      state.selection_end = caret;
      state.selection_direction = dom::SelectionDirection::kNone;
    };

    if (key == "Enter" && is_input) {
      if (dom::Element* form = html::OwnerForm(*control)) {
        // Implicit submission uses the form's default button (HTML
        // §4.10.22.2) as the submitter, so its name/value and form*
        // overrides apply.  The spec fires a click at that button; this
        // engine submits directly with it as the submitter (documented
        // approximation -- no synthetic activation event).
        SubmitForm(tab_id, form, html::DefaultButton(*form));
      }
      return not_canceled;
    }
    if (key == "Backspace" || key == "Delete") {
      std::size_t from = state.selection_start;
      std::size_t to = state.selection_end;
      if (from == to) {
        if (key == "Backspace") {
          from = dom::PreviousUtf16Offset(value, from);
        } else {
          to = dom::NextUtf16Offset(value, to);
        }
      }
      if (from != to) {
        value = dom::ReplaceUtf16Range(value, from, to, {});
        store_value(value);
        state.selection_start = from;
        state.selection_end = from;
        state.selection_direction = dom::SelectionDirection::kNone;
      }
    } else if (key == "ArrowLeft" || key == "ArrowRight" || key == "Home" || key == "End") {
      std::size_t caret = state.selection_start;
      if (key == "ArrowLeft") {
        caret = state.selection_start != state.selection_end
                    ? state.selection_start
                    : dom::PreviousUtf16Offset(value, state.selection_start);
      } else if (key == "ArrowRight") {
        caret = state.selection_start != state.selection_end
                    ? state.selection_end
                    : dom::NextUtf16Offset(value, state.selection_end);
      } else if (key == "Home") {
        caret = 0;
      } else if (key == "End") {
        caret = length;
      }
      // Home moves to offset 0.  Both are whole-value boundaries; line-based
      // Home/End for textarea is not implemented (documented).
      state.selection_start = caret;
      state.selection_end = caret;
      state.selection_direction = dom::SelectionDirection::kNone;
    } else if (key == "Enter") {
      replace_selection("\n");
    } else if (IsPrintableKey(key)) {
      replace_selection(key);
    } else {
      // Named keys without a default text action (ArrowUp/Down, PageUp/Down,
      // Escape, function keys, ...) must not be inserted as literal text.
      return not_canceled;
    }
    if (value_changed) {
      // The sanitizer may have dropped the inserted text (a number control
      // rejecting "x"); keep the caret inside the value it actually produced.
      const std::size_t new_length = dom::Utf16Length(html::InputValue(*control));
      state.selection_start = std::min(state.selection_start, new_length);
      state.selection_end = std::min(state.selection_end, new_length);
      if (tab->script_runtime != nullptr) {
        // Fire the bubbling "input" event (oninput / addEventListener('input')).
        tab->script_runtime->DispatchInputEvent(*control);
      }
    }
  }
  // Reflect DOM changes from the keydown/input handlers or the value edit
  // (the layout rebuilds and the UI repaints on the next StateChanged).  The
  // value edit always rebuilds, even on pages with no scripts.
  bool key_style_changed = value_changed;
  bool key_dom_changed = value_changed;
  if (!value_changed && tab->script_runtime != nullptr) {
    key_style_changed = ReflectScriptStyleMutations(*tab);
    key_dom_changed = tab->script_runtime->TakeDomDirty();
  }
  if (value_changed) {
    // The value edit is geometry the layout reads outside the cascade: it
    // always rebuilds (the scoped path cannot see it).  The key handlers ran
    // too and may have changed document.title.
    tab->page->ReapplyStyles();
    RefreshTabTitle(*tab);
  }
  if (key_style_changed || key_dom_changed) {
    SchedulePendingImageFetch(*tab);
  }
  // An input handler may have submitted the form.
  DrainPendingFormSubmission(*tab);
  return not_canceled;
}

bool ContainsNode(const dom::Node& root, const dom::Node* target)
{
  if (target == nullptr) {
    return false;
  }
  std::vector<const dom::Node*> stack{&root};
  while (!stack.empty()) {
    const dom::Node* node = stack.back();
    stack.pop_back();
    if (node == target) {
      return true;
    }
    for (const dom::Node* child : node->ChildNodes()) {
      stack.push_back(child);
    }
  }
  return false;
}

void BrowserController::SubmitForm(int tab_id, dom::Element* form, dom::Element* submitter)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr || form == nullptr || tab->page == nullptr) {
    return;
  }
  // Keep the Page alive while the submission runs: a scripted submission (a
  // form.submit() issued from a submit/invalid handler) replaces the document
  // before this call returns.
  const std::shared_ptr<renderer::Page> page_lock_keepalive = tab->page;
  const std::string document_url = tab->url;
  const bool navigated =
      RunFormSubmission(*tab, *page_lock_keepalive, document_url, form, submitter, false);
  // A navigation replaces the document, invalidating every queued element
  // pointer; only drain while the submitting document is still current.
  if (!navigated) {
    DrainPendingFormSubmission(*tab);
  }
}

bool BrowserController::RunFormSubmission(Tab& tab,
                                          renderer::Page& page,
                                          const std::string& document_url,
                                          dom::Element* form,
                                          dom::Element* submitter,
                                          bool scripted)
{
  FormSubmissionRequest request{form, submitter, scripted};
  // A form.submit()/requestSubmit() issued by a handler during this submission
  // supersedes it (the spec's planned navigation).  Returns true when a newer
  // request was taken over.
  const auto supersede = [&tab, &request]() -> bool {
    if (tab.pending_form_submission.form == nullptr) {
      return false;
    }
    request.form = tab.pending_form_submission.form;
    request.submitter = tab.pending_form_submission.submitter;
    request.scripted = !tab.pending_form_submission.request_submit;
    tab.pending_form_submission = {};
    return true;
  };
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (request.form == nullptr) {
      return false;
    }
    const dom::Document* document = page.document();
    if (document == nullptr || !ContainsNode(*document, request.form) ||
        (request.submitter != nullptr && !ContainsNode(*document, request.submitter))) {
      // The element was removed by a script between the request and its
      // execution.  Dropping the request is the only safe option: the pointer
      // is no longer part of the live tree.
      return false;
    }
    std::optional<PreparedFormSubmission> prepared;
    {
      const std::unique_lock<std::recursive_mutex> dom_lock = page.AcquireDomLock();
      if (!request.scripted) {
        // Interactive validation comes before the submit event (HTML
        // §4.10.22.3).  A scripted submission during either phase supersedes
        // this one.
        if (supersede()) {
          continue;
        }
        const bool no_validate =
            request.form->HasAttribute("novalidate") ||
            (request.submitter != nullptr && request.submitter->HasAttribute("formnovalidate"));
        if (!no_validate) {
          std::vector<dom::Element*> invalid;
          if (tab.script_runtime != nullptr) {
            invalid = tab.script_runtime->InvalidFormControls(*request.form);
            for (dom::Element* control : invalid) {
              tab.script_runtime->DispatchInvalidEvent(*control);
            }
          } else {
            invalid = html::InvalidControls(*request.form);
          }
          if (!invalid.empty()) {
            // Interactive validation reports the problem at the first invalid
            // control (focus; the UI paints the caret there).
            FocusControl(&tab, invalid.front(), &page);
            if (supersede()) {
              continue;
            }
            return false;
          }
        }
        if (supersede()) {
          continue;
        }
        if (tab.script_runtime != nullptr &&
            !tab.script_runtime->DispatchCancelableEvent(*request.form, "submit")) {
          // preventDefault canceled this submission.  A form.submit() issued
          // by the handler still navigates (submit() skips the event).
          if (supersede()) {
            continue;
          }
          return false;
        }
        if (supersede()) {
          continue;
        }
      }
      PreparedFormSubmission candidate;
      if (!PrepareFormSubmission(document_url, *request.form, request.submitter, &candidate)) {
        return false;
      }
      // No script runs between the last supersede check and here, so the
      // prepared submission is still the newest one.
      prepared = std::move(candidate);
    }
    return ApplyFormSubmission(tab, std::move(*prepared));
  }
  // A pathological submit-handler loop: drop anything still queued.
  tab.pending_form_submission = {};
  return false;
}

bool BrowserController::PrepareFormSubmission(const std::string& document_url,
                                              dom::Element& form,
                                              dom::Element* submitter,
                                              PreparedFormSubmission* out)
{
  // Attributes for form submission: the submitter's form* attributes win over
  // the form's (HTML §4.10.19.6).
  const auto attr_or_form = [&form, submitter](std::string_view submitter_attr,
                                               std::string_view form_attr) -> std::string {
    if (submitter != nullptr && submitter->HasAttribute(submitter_attr)) {
      return std::string(submitter->GetAttribute(submitter_attr).value_or(""));
    }
    return std::string(form.GetAttribute(form_attr).value_or(""));
  };

  // The action of the element: submitter's formaction, else the form's action.
  std::string action = attr_or_form("formaction", "action");
  std::string target = ResolveUrlAgainstBase(action, document_url);
  if (target.empty()) {
    target = document_url;
  }

  // Method: enumerated attribute.  get/post are implemented; dialog is
  // explicitly NOT IMPLEMENTED (there is no dialog element/close algorithm),
  // so it falls back to GET rather than silently posting.
  const std::string method_value =
      base::ToLower(std::string(Trim(attr_or_form("formmethod", "method"))));
  const bool post = method_value == "post";

  // Enctype: submitter's formenctype, else the form's enctype (invalid/missing
  // value default: application/x-www-form-urlencoded).
  const FormEnctype enctype = ParseEnctype(attr_or_form("formenctype", "enctype"));

  std::vector<html::FormEntry> entries = html::ConstructEntryList(form, submitter);
  std::string body;
  std::string content_type;
  if (post) {
    out->method = "POST";
    switch (enctype) {
    case FormEnctype::kMultipart: {
      const std::string boundary = GenerateMultipartBoundary();
      body = EncodeMultipart(entries, boundary);
      content_type = "multipart/form-data; boundary=" + boundary;
      break;
    }
    case FormEnctype::kTextPlain:
      body = EncodePlainText(entries);
      content_type = "text/plain";
      break;
    case FormEnctype::kUrlEncoded:
      body = EncodeUrlEncoded(entries);
      content_type = "application/x-www-form-urlencoded";
      break;
    }
  } else {
    // GET ("mutate action URL", including the dialog fallback): the entry list
    // always travels as an application/x-www-form-urlencoded query string,
    // whatever the enctype says.
    out->method.clear();
    target = AppendQuery(std::move(target), EncodeUrlEncoded(entries));
  }

  out->url = std::move(target);
  out->body = std::move(body);
  out->content_type = std::move(content_type);
  out->target = std::string(Trim(attr_or_form("formtarget", "target")));
  // Password manager: capture the login this submission would offer to save
  // (the origin gate below skips file:/data: documents).
  out->credential = CredentialFromForm(form);
  return true;
}

bool BrowserController::ApplyFormSubmission(Tab& tab, PreparedFormSubmission submission)
{
  // Publish the captured login (if any) before navigating: the save prompt
  // belongs to this submission, not to whatever page comes next.
  if (submission.credential.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tab.origin.empty() && tab.origin != "null") {
      submission.credential->origin = tab.origin;
      tab.pending_credential = std::move(submission.credential);
    }
  }
  // Navigate outside the DOM lock.  NavigateWithRequestBody() replaces the
  // tab's Page, which destroys the Page the lock was taken on; unlocking it
  // afterwards was a use-after-free on the destroyed Page's mutex (reported by
  // ThreadSanitizer, and undefined behaviour per the standard).
  //
  // _blank opens a new tab through the normal navigation path (history and
  // cookies stay browser-side).  _self, _parent and _top all map to the
  // current tab, and a named target falls back to it as well: the engine has no
  // frame tree / named browsing contexts yet (documented approximation).
  const std::string target_keyword = base::ToLower(submission.target);
  if (target_keyword == "_blank") {
    const int new_tab = NewTab();
    static_cast<void>(NavigateWithRequestBody(new_tab,
                                              submission.url,
                                              std::move(submission.method),
                                              std::move(submission.body),
                                              std::move(submission.content_type)));
    return false;
  }
  static_cast<void>(NavigateWithRequestBody(tab.id,
                                            submission.url,
                                            std::move(submission.method),
                                            std::move(submission.body),
                                            std::move(submission.content_type)));
  return true;
}

void BrowserController::DrainPendingFormSubmission(Tab& tab)
{
  for (int attempt = 0; attempt < 8; ++attempt) {
    const ScriptRequestedFormSubmission request = tab.pending_form_submission;
    tab.pending_form_submission = {};
    if (request.form == nullptr) {
      return;
    }
    const std::shared_ptr<renderer::Page> page = tab.page;
    if (page == nullptr) {
      return;
    }
    const std::string document_url = tab.url;
    const bool navigated = RunFormSubmission(
        tab, *page, document_url, request.form, request.submitter, !request.request_submit);
    // Any navigation replaces the document (and invalidates the queued
    // pointers); stop rather than interpreting stale state.
    if (navigated || tab.page != page) {
      return;
    }
  }
  tab.pending_form_submission = {};
}

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

