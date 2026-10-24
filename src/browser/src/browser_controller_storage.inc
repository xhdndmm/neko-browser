// ---------------------------------------------------------------------------
// Bookmarks / console
// ---------------------------------------------------------------------------

base::Result<std::string> BrowserController::BookmarkActive(const std::string& folder)
{
  Tab* tab = ActiveTab();
  if (tab == nullptr || tab->url.empty()) {
    return base::Err(base::Error::InvalidArgument("nothing to bookmark"));
  }
  return bookmarks_.Add(tab->url, tab->title.empty() ? tab->url : tab->title, folder, NowUnix());
}

void BrowserController::LogConsole(std::string_view level, std::string_view message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  console_log_.push_back(ConsoleEntry{std::string(level), std::string(message), NowUnix()});
  if (console_log_.size() > 500) {
    console_log_.erase(console_log_.begin(),
                       console_log_.begin() + static_cast<ptrdiff_t>(console_log_.size() - 500));
  }
}

void BrowserController::ClearNetworkLog()
{
  std::lock_guard<std::mutex> lock(mutex_);
  network_log_.clear();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

base::Result<void> BrowserController::Load()
{
  auto r1 = cookies_.Load();
  auto r2 = history_.Load();
  auto r3 = bookmarks_.Load();
  auto r4 = local_storage_.Load();
  auto r5 = indexed_db_.Load();
  auto r6 = preferences_.Load();
  auto r7 = passwords_.Load();
  if (!r1)
    return r1.error();
  if (!r2)
    return r2.error();
  if (!r3)
    return r3.error();
  if (!r4)
    return r4.error();
  if (!r5)
    return r5.error();
  if (!r6)
    return r6.error();
  return r7;
}

base::Result<void> BrowserController::Save()
{
  cookies_.PurgeExpired(NowUnix());
  auto r1 = cookies_.Save();
  auto r2 = history_.Save();
  auto r3 = bookmarks_.Save();
  auto r4 = local_storage_.Save();
  auto r5 = indexed_db_.Save();
  auto r6 = preferences_.Save();
  if (!r1)
    return r1.error();
  if (!r2)
    return r2.error();
  if (!r3)
    return r3.error();
  if (!r4)
    return r4.error();
  if (!r5)
    return r5.error();
  return r6;
}

// ---------------------------------------------------------------------------
// Worker-thread store operations
// ---------------------------------------------------------------------------

base::Result<Download> BrowserController::StartDownload(const url::Url& url,
                                                        std::string_view cookie_header)
{
  return downloads_.Start(url, cookie_header);
}

std::string BrowserController::GetPreference(std::string_view key, std::string_view fallback) const
{
  // PreferencesStore is internally synchronized; safe from any thread.
  return preferences_.Get(key, fallback);
}

std::vector<std::pair<std::string, std::string>> BrowserController::SnapshotPreferences() const
{
  return preferences_.All();
}

void BrowserController::SetPreference(std::string_view key, std::string_view value)
{
  preferences_.Set(key, value);
  const auto saved = preferences_.Save();
  if (!saved) {
    NEKO_LOG_WARNING("failed to persist preference '" + std::string(key) +
                     "': " + saved.error().message());
  }
}

size_t BrowserController::ClearFinishedDownloads()
{
  return downloads_.ClearFinished();
}

void BrowserController::RemoveHistoryEntry(const std::string& url)
{
  if (history_.Remove(url)) {
    const auto saved = history_.Save();
    if (!saved) {
      NEKO_LOG_WARNING("failed to persist history removal: " + saved.error().message());
    }
  }
}

void BrowserController::ClearHistory()
{
  history_.Clear();
  const auto saved = history_.Save();
  if (!saved) {
    NEKO_LOG_WARNING("failed to persist history clear: " + saved.error().message());
  }
}

PointInfo BrowserController::QueryPoint(int tab_id, float doc_x, float doc_y) const
{
  // Grab the page handle and URL under the controller lock, then release it:
  // Page::ElementAt takes the Page's own DOM lock, and the lock order is Page
  // before controller, never the other way around (see mutex_ comment).
  std::shared_ptr<renderer::Page> page;
  std::string base_url;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(
        tabs_.begin(), tabs_.end(), [tab_id](const auto& t) { return t->id == tab_id; });
    if (it == tabs_.end()) {
      return {};
    }
    page = (*it)->page;
    base_url = (*it)->url;
  }
  if (page == nullptr) {
    // Renderer-process mode (or non-HTML content): no in-process DOM to
    // hit-test, so the menu falls back to its basic entries.
    return {};
  }
  // Hit-test and read the element's attributes while holding the page's DOM
  // lock: ElementAt hands back a raw pointer into the document, and the worker
  // thread may replace or mutate that document concurrently (navigation,
  // scripts), which previously left the pointer dangling before
  // HyperlinkTarget walked it.  The lock is recursive, so ElementAt may
  // re-acquire it internally.
  const std::unique_lock<std::recursive_mutex> dom_lock = page->AcquireDomLock();
  const dom::Element* element = page->ElementAt(doc_x, doc_y);
  if (element == nullptr) {
    return {};
  }
  PointInfo info;
  if (const std::optional<std::string> link = HyperlinkTarget(element, base_url)) {
    info.link_url = *link;
  }
  if (element->tag_name() == "img") {
    const auto src = element->GetAttribute("src");
    if (src.has_value()) {
      if (const std::optional<std::string> resolved = ResolveReference(*src, base_url)) {
        info.image_url = *resolved;
      }
    }
  }
  return info;
}

void BrowserController::AutofillLoginForms(renderer::Page& page, const std::string& origin)
{
  if (origin.empty() || origin == "null") {
    return;
  }
  const std::vector<storage::Credential> saved = passwords_.ForOrigin(origin);
  if (saved.empty()) {
    return;
  }
  const std::unique_lock<std::recursive_mutex> dom_lock = page.AcquireDomLock();
  dom::Document* doc = page.document();
  if (doc == nullptr) {
    return;
  }
  const std::vector<dom::Element*> inputs = CollectInputs(*doc);
  for (dom::Element* input : inputs) {
    if (InputType(*input) != "password") {
      continue;
    }
    const std::string existing = html::InputValue(*input);
    if (!existing.empty()) {
      return; // the page (or the user) already filled this form: leave it alone
    }
    const storage::Credential& credential = saved.front();
    html::SetInputValue(*input, credential.password);
    if (dom::Element* username = FindUsernameInput(inputs, input); username != nullptr) {
      const std::string existing_user = html::InputValue(*username);
      if (existing_user.empty() && !credential.username.empty()) {
        html::SetInputValue(*username, credential.username);
      }
    }
    return; // first login form only
  }
}

std::optional<PendingCredential>
BrowserController::CredentialFromForm(const dom::Element& form) const
{
  const std::vector<dom::Element*> inputs = CollectInputs(const_cast<dom::Element&>(form));
  dom::Element* password = nullptr;
  std::string password_value;
  for (dom::Element* input : inputs) {
    if (InputType(*input) != "password") {
      continue;
    }
    std::string value = html::InputValue(*input);
    if (!value.empty()) {
      password = input;
      password_value = std::move(value);
      break;
    }
  }
  if (password == nullptr) {
    return std::nullopt;
  }
  PendingCredential credential;
  if (const dom::Element* username = FindUsernameInput(inputs, password); username != nullptr) {
    credential.username = html::InputValue(*username);
  }
  credential.password = std::move(password_value);
  return credential;
}

std::vector<SavedLogin> BrowserController::SavedLogins() const
{
  const std::vector<storage::Credential> all = passwords_.All();
  std::vector<SavedLogin> out;
  out.reserve(all.size());
  for (const storage::Credential& credential : all) {
    out.push_back(SavedLogin{credential.origin, credential.username});
  }
  return out;
}

bool BrowserController::RemoveSavedLogin(const std::string& origin, const std::string& username)
{
  const bool removed = passwords_.Remove(origin, username);
  if (removed) {
    const auto saved = passwords_.Save();
    if (!saved.has_value()) {
      NEKO_LOG_WARNING("failed to persist removed login: " + saved.error().message());
    }
  }
  return removed;
}

void BrowserController::SavePendingCredential(int tab_id)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return;
  }
  std::optional<PendingCredential> credential;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    credential = std::move(tab->pending_credential);
    tab->pending_credential.reset();
  }
  if (!credential.has_value()) {
    return;
  }
  passwords_.Add(credential->origin, credential->username, credential->password);
  const auto saved = passwords_.Save();
  if (!saved.has_value()) {
    NEKO_LOG_WARNING("failed to persist saved login: " + saved.error().message());
  }
}

void BrowserController::DismissPendingCredential(int tab_id)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  tab->pending_credential.reset();
}

void BrowserController::RemoveBookmark(const std::string& url)
{
  const auto all = bookmarks_.All();
  for (const auto& b : all) {
    if (b.url == url) {
      (void)bookmarks_.Remove(b.id);
      break;
    }
  }
  (void)Save();
}

void BrowserController::ClearAllStorage()
{
  cookies_.Clear();
  history_.Clear();
  bookmarks_.Clear();
  local_storage_.ClearAll();
  indexed_db_.ClearAll();
  passwords_.Clear();
  ClearNetworkLog();
  (void)Save();
}

std::string BrowserController::CookieHeader(const url::Url& url,
                                            int64_t now,
                                            const storage::CookieSameSiteContext& same_site) const
{
  return cookies_.CookieHeaderFor(url, now, same_site);
}

// ---------------------------------------------------------------------------
// DevTools output
// ---------------------------------------------------------------------------

std::string BrowserController::DumpDom() const
{
  // Lock order (see the note on BrowserController::mutex_): the Page lock is
  // always taken *before* the controller mutex, never the other way round.  Grab
  // the page handle under the controller lock, then dump with the controller
  // lock released -- Page::DumpDom takes the page lock internally, and taking
  // it while holding mutex_ would invert the order against ProduceFrame.
  std::shared_ptr<renderer::Page> page;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size()))
      return "";
    page = tabs_[static_cast<size_t>(active_tab_)]->page;
  }
  return page != nullptr ? page->DumpDom() : std::string();
}

std::string BrowserController::DumpNetworkLog() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::string out;
  for (const NetworkLogEntry& entry : network_log_) {
    out += entry.method;
    out += ' ';
    out += entry.url;
    out += " -> ";
    if (entry.error.empty()) {
      out += std::to_string(entry.status);
      out += " (";
      out += std::to_string(entry.bytes);
      out += " bytes, ";
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.1f", entry.elapsed_ms);
      out += buf;
      out += " ms)";
    } else {
      out += "ERROR: ";
      out += entry.error;
    }
    out += '\n';
  }
  return out;
}

// Fetches the page's external stylesheets (<link rel="stylesheet" href=...>)
// in parallel on a thread pool, parses each body, and registers the parsed
// sheets on the page (re-running the cascade).  The fetch hook must be
// thread-safe (see FetchPageImages).  Only https/http stylesheet URLs are
// fetched; data: and empty hrefs are skipped.
void FetchExternalStylesheets(renderer::Page& page,
                              const std::string& base_url,
                              const BrowserController::FetchFn& fetch,
                              base::ThreadPool& pool)
{
  dom::Document* doc = page.document();
  if (doc == nullptr) {
    return;
  }
  const base::Result<url::Url> base = url::Url::Parse(base_url);

  // Collect <link rel=stylesheet> in document order (pre-order traversal).
  // The stack pops LIFO, so children are pushed in reverse to yield forward
  // order on pop.
  std::vector<dom::Element*> links;
  std::vector<dom::Node*> stack;
  for (auto it = doc->ChildNodes().begin(); it != doc->ChildNodes().end(); ++it) {
    stack.push_back(*it);
  }
  while (!stack.empty()) {
    dom::Node* node = stack.back();
    stack.pop_back();
    if (node->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element* element = static_cast<dom::Element*>(node);
    if (element->tag_name() == "link" && element->HasAttribute("rel") &&
        element->HasAttribute("href")) {
      const std::optional<std::string_view> rel = element->GetAttribute("rel");
      if (rel.has_value() && base::AsciiEqualsIgnoreCase(*rel, "stylesheet")) {
        links.push_back(element);
      }
    }
    // Reverse iteration so the stack pops children in document order.
    std::vector<dom::Node*> children;
    for (dom::Node* child : node->ChildNodes()) {
      children.push_back(child);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  if (links.empty()) {
    return;
  }

  // Resolve hrefs (document order preserved).
  std::vector<std::string> urls;
  std::unordered_set<std::string> seen_urls;
  urls.reserve(links.size());
  seen_urls.reserve(links.size());
  for (dom::Element* element : links) {
    const std::optional<std::string_view> href = element->GetAttribute("href");
    base::Result<url::Url> target = url::Url::Parse(*href);
    if (base.has_value()) {
      target = url::Url::Parse(*href, base.value());
    }
    if (!target.has_value()) {
      continue;
    }
    // http/https go to the network; data: stylesheets are decoded locally by
    // the network layer (RFC 2397).  Everything else is skipped for now.
    const std::string& scheme = target.value().scheme();
    if (scheme != "http" && scheme != "https" && scheme != "data") {
      continue;
    }
    const std::string serialized = target.value().Serialize();
    if (seen_urls.insert(serialized).second) {
      urls.push_back(serialized);
    }
  }
  if (urls.empty()) {
    return;
  }

  auto fetch_and_parse = [&fetch](const std::string& url) -> base::Result<css::StyleSheet> {
    const base::Result<url::Url> parsed = url::Url::Parse(url);
    if (!parsed.has_value()) {
      return base::Err(base::Error::InvalidArgument("invalid stylesheet URL"));
    }
    const auto response = fetch(parsed.value(), /*cookie_header=*/{}, /*extra_headers=*/{});
    if (!response) {
      return base::Err(response.error());
    }
    css::StyleSheet sheet = css::ParseStyleSheet(response.value().body);
    const std::string resolved_url =
        response.value().final_url.empty() ? url : response.value().final_url;
    const base::Result<url::Url> stylesheet_url = url::Url::Parse(resolved_url);
    if (stylesheet_url.has_value()) {
      for (css::FontFaceRule& face : sheet.font_faces) {
        const base::Result<url::Url> font_url =
            url::Url::Parse(face.src_url, stylesheet_url.value());
        if (font_url.has_value()) {
          face.src_url = font_url.value().Serialize();
        }
      }
    }
    return base::Ok(std::move(sheet));
  };

  std::vector<css::StyleSheet> sheets;
  if (urls.size() == 1) {
    auto sheet = fetch_and_parse(urls[0]);
    if (!sheet) {
      NEKO_LOG_WARNING("css: fetch/parse failed for " + urls[0]);
      return;
    }
    sheets.push_back(std::move(sheet.value()));
  } else {
    std::vector<std::future<base::Result<css::StyleSheet>>> futures;
    futures.reserve(urls.size());
    for (const std::string& url : urls) {
      futures.push_back(pool.Submit([&fetch_and_parse, url]() { return fetch_and_parse(url); }));
    }
    for (std::size_t i = 0; i < urls.size(); ++i) {
      auto sheet = futures[i].get();
      if (!sheet) {
        NEKO_LOG_WARNING("css: fetch/parse failed for " + urls[i]);
        continue;
      }
      sheets.push_back(std::move(sheet.value()));
    }
  }
  if (!sheets.empty()) {
    NEKO_LOG_INFO("css: applied " + std::to_string(sheets.size()) + " external stylesheet(s)");
    page.SetExternalStylesheets(std::move(sheets));
  }
}

// Fetches and registers @font-face web fonts declared by the page's
// stylesheets.  Runs after external stylesheets are applied so their
// declarations are visible; fonts load in parallel and each registration
// invalidates the layout caches (a final ReapplyStyles rebuilds text).
void FetchWebFonts(renderer::Page& page,
                   const std::string& base_url,
                   const BrowserController::FetchFn& fetch,
                   base::ThreadPool& pool)
{
  const std::vector<css::FontFaceRule> faces = page.FontFaces();
  if (faces.empty()) {
    return;
  }
  const base::Result<url::Url> base = url::Url::Parse(base_url);

  struct PendingFont
  {
    css::FontFaceRule rule;
    std::string absolute_url;
    bool seen = false; // duplicate src within this page
  };
  std::vector<PendingFont> pending;
  for (const css::FontFaceRule& face : faces) {
    if (face.src_url.empty() || face.family.empty()) {
      continue;
    }
    const base::Result<url::Url> target = base.has_value()
                                              ? url::Url::Parse(face.src_url, base.value())
                                              : url::Url::Parse(face.src_url);
    if (!target.has_value()) {
      continue;
    }
    const std::string absolute = target.value().Serialize();
    if (!page.ClaimWebFont(absolute)) {
      // Registered or attempted by an earlier load pass — skip the refetch.
      // A URL that failed (jd.com's fonts live on a host whose DNS record is
      // gone) used to be retried, and re-warned, once per stylesheet pass.
      continue;
    }
    bool dup = false;
    for (PendingFont& p : pending) {
      if (p.absolute_url == absolute) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      pending.push_back(PendingFont{face, absolute, false});
    }
  }
  if (pending.empty()) {
    return;
  }

  auto fetch_bytes = [&fetch](const std::string& url) -> base::Result<std::vector<uint8_t>> {
    const base::Result<url::Url> parsed = url::Url::Parse(url);
    if (!parsed.has_value()) {
      return base::Err(base::Error::InvalidArgument("invalid font URL"));
    }
    const auto response = fetch(parsed.value(), /*cookie_header=*/{}, /*extra_headers=*/{});
    if (!response.has_value()) {
      return base::Err(response.error());
    }
    if (response.value().status_code >= 400) {
      return base::Err(
          base::Error::InvalidArgument("HTTP " + std::to_string(response.value().status_code)));
    }
    std::vector<uint8_t> bytes(response.value().body.begin(), response.value().body.end());
    return base::Ok(std::move(bytes));
  };

  bool loaded_any = false;
  auto load_font = [&page, &pending, &loaded_any](std::size_t index,
                                                  base::Result<std::vector<uint8_t>> bytes) {
    if (!bytes.has_value()) {
      NEKO_LOG_WARNING("font: fetch failed for " + pending[index].absolute_url + ": " +
                       bytes.error().message());
      return;
    }
    if (page.LoadWebFont(pending[index].rule.family,
                         pending[index].rule.weight,
                         pending[index].rule.italic,
                         pending[index].absolute_url,
                         std::move(bytes.value()))) {
      loaded_any = true;
      NEKO_LOG_INFO("font: registered '" + pending[index].rule.family + "' <- " +
                    pending[index].absolute_url);
    }
  };
  if (pending.size() == 1 || pool.thread_count() < 2) {
    for (std::size_t i = 0; i < pending.size(); ++i) {
      load_font(i, fetch_bytes(pending[i].absolute_url));
    }
  } else {
    std::vector<std::future<base::Result<std::vector<uint8_t>>>> futures;
    futures.reserve(pending.size());
    for (const PendingFont& item : pending) {
      futures.push_back(
          pool.Submit([&fetch_bytes, url = item.absolute_url]() { return fetch_bytes(url); }));
    }
    for (std::size_t i = 0; i < pending.size(); ++i) {
      load_font(i, futures[i].get());
    }
  }
  if (loaded_any) {
    page.ReapplyStyles();
  }
}

void FetchPageImages(renderer::Page& page,
                     const std::string& base_url,
                     const BrowserController::FetchFn& fetch,
                     base::ThreadPool& pool)
{
  const base::Result<url::Url> base = url::Url::Parse(base_url);

  // Depth-first walk collecting <img src> and CSS background-image
  // subresources.  background-image URLs come from the computed style (the
  // cascade has run by the time this is called, after scripts).  The claim
  // marks each (element, URL) pair: repeat passes — scheduled from the script
  // pump for lazy-loaded sources — only process what changed, so the steady
  // state costs one DOM walk and no requests.
  struct PendingImage
  {
    const dom::Element* element = nullptr;
    std::string url;
  };
  struct ImageGroup
  {
    std::string url;
    std::vector<const dom::Element*> elements;
  };
  std::vector<PendingImage> pending;
  const auto collect_url = [&](const dom::Element* element, const std::string& raw_url) {
    if (raw_url.empty()) {
      return;
    }
    base::Result<url::Url> target = url::Url::Parse(raw_url);
    if (base.has_value()) {
      target = url::Url::Parse(raw_url, base.value());
    }
    if (!target.has_value()) {
      NEKO_LOG_WARNING("img: cannot resolve url \"" + raw_url + "\"");
      return;
    }
    pending.push_back(PendingImage{element, target.value().Serialize()});
  };
  for (const auto& [element, raw_url] : page.ClaimPendingImageSources()) {
    collect_url(element, raw_url);
  }
  if (pending.empty()) {
    return;
  }

  // A page commonly reuses the same image in several cards or as both an
  // element image and a background. Fetch and decode each URL once, while
  // retaining every element that needs the decoded result.
  std::vector<ImageGroup> groups;
  std::unordered_map<std::string, std::size_t> group_by_url;
  groups.reserve(pending.size());
  for (const PendingImage& item : pending) {
    const auto [it, inserted] = group_by_url.emplace(item.url, groups.size());
    if (inserted) {
      groups.push_back(ImageGroup{item.url, {}});
    }
    groups[it->second].elements.push_back(item.element);
  }

  // A decoded image plus, for animated GIFs, the full frame set (the Image
  // carries the first frame).
  struct DecodedImage
  {
    image::Image image;
    std::shared_ptr<image::GifAnimation> animation;
  };
  // SVG <text> needs glyph outlines; the renderer's font stack is the same one
  // the page uses (including @font-face web fonts), so page SVG images get real
  // text instead of nothing.
  const image::SvgTextShaper svg_shaper = page.MakeSvgTextShaper();
  // Decodes an image from raw bytes (shared by the network and data: paths).
  auto decode_bytes = [&svg_shaper](const std::string& body) -> base::Result<DecodedImage> {
    DecodedImage out;
    if (image::IsGif(body)) {
      // Animated GIF: decode every frame; keep them for playback when the
      // GIF has more than one.  A static GIF falls through to DecodeImage.
      auto anim = image::DecodeGifAnimation(body);
      if (!anim.has_value()) {
        return base::Err(anim.error());
      }
      if (anim.value().frames.size() > 1) {
        out.animation = std::make_shared<image::GifAnimation>(std::move(anim.value()));
        out.image.width = out.animation->width;
        out.image.height = out.animation->height;
        out.image.rgba = out.animation->frames[0].rgba;
        return out;
      }
      // Single-frame GIF: fall back to the plain decode path below.
    }
    auto decoded = image::DecodeImage(body, svg_shaper);
    if (!decoded.has_value()) {
      return base::Err(decoded.error());
    }
    out.image = std::move(decoded.value());
    return out;
  };
  auto fetch_and_decode = [&fetch,
                           &decode_bytes](const std::string& url) -> base::Result<DecodedImage> {
    const base::Result<url::Url> parsed = url::Url::Parse(url);
    if (!parsed.has_value()) {
      return base::Err(base::Error::InvalidArgument("invalid image URL"));
    }
    // data: URLs never touch the network, but they go through the same
    // decoder the network layer uses — an injected fetcher (tests) would not
    // know how to answer one, so they are resolved before it is consulted.
    const base::Result<network::HttpResponse> response =
        parsed.value().scheme() == "data"
            ? network::DecodeDataUrl(parsed.value().Serialize(/*include_fragment=*/false))
            : fetch(parsed.value(), /*cookie_header=*/{}, /*extra_headers=*/{});
    if (!response) {
      return base::Err(response.error());
    }
    return decode_bytes(response.value().body);
  };

  if (groups.size() == 1) {
    // A single image: do it inline (no thread-pool overhead).
    auto decoded = fetch_and_decode(groups[0].url);
    if (!decoded) {
      NEKO_LOG_WARNING("img: fetch/decode failed for " + groups[0].url);
      // The page sees the failure (browsers fire the element's "error" event;
      // sites hang fallback/placeholder logic off it).
      for (const dom::Element* element : groups[0].elements) {
        page.NoteImageLoadFailed(element);
      }
      return;
    }
    page.SetElementImages(groups[0].elements, decoded.value().image, decoded.value().animation);
    NEKO_LOG_INFO("img: fetched " + groups[0].url + "; attached to " +
                  std::to_string(groups[0].elements.size()) + " element(s)");
    return;
  }

  std::vector<std::future<base::Result<DecodedImage>>> futures;
  futures.reserve(groups.size());
  for (const ImageGroup& group : groups) {
    futures.push_back(
        pool.Submit([&fetch_and_decode, url = group.url]() { return fetch_and_decode(url); }));
  }
  for (std::size_t i = 0; i < groups.size(); ++i) {
    auto decoded = futures[i].get();
    if (!decoded) {
      NEKO_LOG_WARNING("img: fetch/decode failed for " + groups[i].url);
      for (const dom::Element* element : groups[i].elements) {
        page.NoteImageLoadFailed(element);
      }
      continue;
    }
    page.SetElementImages(groups[i].elements, decoded.value().image, decoded.value().animation);
    NEKO_LOG_INFO("img: fetched " + groups[i].url + "; attached to " +
                  std::to_string(groups[i].elements.size()) + " element(s)");
  }
}

void FetchPageVideos(renderer::Page& page,
                     const std::string& base_url,
                     const BrowserController::FetchFn& fetch,
                     base::ThreadPool& pool)
{
  const base::Result<url::Url> base = url::Url::Parse(base_url);

  struct PendingVideo
  {
    const dom::Element* element = nullptr;
    std::string url;
    bool autoplay = false;
    bool loop = false;
  };
  std::vector<PendingVideo> pending;
  for (const renderer::Page::VideoSource& source : page.VideoSources()) {
    base::Result<url::Url> target = url::Url::Parse(source.url);
    if (base.has_value()) {
      target = url::Url::Parse(source.url, base.value());
    }
    if (target.has_value()) {
      pending.push_back(
          PendingVideo{source.element, target.value().Serialize(), source.autoplay, source.loop});
    } else {
      NEKO_LOG_WARNING("video: cannot resolve url \"" + source.url + "\"");
    }
  }
  if (pending.empty()) {
    return;
  }

  auto fetch_and_decode = [&fetch](const std::string& url) -> base::Result<media::VideoClip> {
    const base::Result<url::Url> parsed = url::Url::Parse(url);
    if (!parsed.has_value()) {
      return base::Err(base::Error::InvalidArgument("invalid video URL"));
    }
    const auto response = fetch(parsed.value(), /*cookie_header=*/{}, /*extra_headers=*/{});
    if (!response) {
      NEKO_LOG_WARNING("video: fetch failed for " + url + ": " + response.error().message());
      return base::Err(response.error());
    }
    // The decoder is budgeted (frame count + total RGBA bytes), so a huge
    // video degrades to a bounded prefix rather than exhausting memory.
    auto clip = media::DecodeVideo(response.value().body);
    if (!clip) {
      NEKO_LOG_WARNING("video: decode failed for " + url + ": " + clip.error().message());
    }
    return clip;
  };

  const auto attach = [&page](const PendingVideo& item, media::VideoClip clip) {
    if (clip.frames.empty()) {
      return;
    }
    auto frames = std::make_shared<std::vector<image::Image>>();
    frames->reserve(clip.frames.size());
    for (media::VideoFrame& frame : clip.frames) {
      frames->push_back(std::move(frame.image));
    }
    renderer::Page::VideoStrip strip;
    strip.frames = std::move(frames);
    strip.frame_rate = clip.frame_rate;
    strip.loop = item.loop;
    const std::size_t frame_count = strip.frames->size();
    const image::Image first_frame = (*strip.frames)[0]; // copy: strip moves below
    page.SetElementVideo(item.element, first_frame, std::move(strip), item.autoplay);
    NEKO_LOG_INFO("video: injected " + item.url + " (" + std::to_string(frame_count) +
                  (clip.truncated ? " frames, budget-truncated)" : " frames)"));
  };

  if (pending.size() == 1) {
    auto decoded = fetch_and_decode(pending[0].url);
    if (!decoded) {
      NEKO_LOG_WARNING("video: fetch/decode failed for " + pending[0].url);
      return;
    }
    attach(pending[0], std::move(decoded.value()));
    return;
  }
  std::vector<std::future<base::Result<media::VideoClip>>> futures;
  futures.reserve(pending.size());
  for (const PendingVideo& item : pending) {
    futures.push_back(
        pool.Submit([&fetch_and_decode, url = item.url]() { return fetch_and_decode(url); }));
  }
  for (std::size_t i = 0; i < pending.size(); ++i) {
    auto decoded = futures[i].get();
    if (!decoded) {
      NEKO_LOG_WARNING("video: fetch/decode failed for " + pending[i].url);
      continue;
    }
    attach(pending[i], std::move(decoded.value()));
  }
}

