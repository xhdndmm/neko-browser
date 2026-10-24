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
