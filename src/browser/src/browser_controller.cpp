#include "neko/browser/browser_controller.h"

#include "neko/base/logging.h"
#include "neko/base/thread_pool.h"
#include "neko/browser/page_scripts.h"
#include "neko/dom/element.h"
#include "neko/image/image.h"
#include "neko/network/http.h"
#include "neko/storage/file_util.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace neko::browser {
namespace {

std::string_view Trim(std::string_view s)
{
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

int64_t NowUnix()
{
  return static_cast<int64_t>(std::time(nullptr));
}

// Lowercases an ASCII string.
std::string ToLower(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  return out;
}

bool StartsWith(std::string_view s, std::string_view prefix)
{
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// True when |bytes| begin with '<' (after optional whitespace/BOM), which we
// take as a hint for HTML content when no Content-Type was provided.
bool LooksLikeHtml(std::string_view bytes)
{
  size_t i = 0;
  if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF &&
      static_cast<uint8_t>(bytes[1]) == 0xBB && static_cast<uint8_t>(bytes[2]) == 0xBF) {
    i = 3;
  }
  while (i < bytes.size() &&
         (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\n' || bytes[i] == '\r')) {
    ++i;
  }
  return i < bytes.size() && bytes[i] == '<';
}

// Copies the GUI-visible state of |tab|.  Called with the controller mutex
// held; the shared payload handles keep the data alive for the caller.
TabSnapshot ToSnapshot(const Tab& tab)
{
  TabSnapshot s;
  s.id = tab.id;
  s.url = tab.url;
  s.title = tab.title;
  s.loading = tab.loading;
  s.content_type = tab.content_type;
  s.page = tab.page;
  s.image = tab.image;
  s.pdf = tab.pdf;
  s.audio = tab.audio;
  s.raw_text = tab.raw_text;
  s.error = tab.error;
  return s;
}

} // namespace

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

BrowserController::BrowserController(std::string profile_dir, FetchFn fetch)
    : profile_dir_(std::move(profile_dir)), fetch_(std::move(fetch)), cookies_(profile_dir_),
      history_(profile_dir_), bookmarks_(profile_dir_), local_storage_(profile_dir_),
      downloads_(profile_dir_ + "/downloads")
{
  // Default fetch: network::HttpGet with the controller-provided cookie
  // header.
  if (!fetch_) {
    fetch_ = [](const url::Url& u, std::string_view cookie_header) {
      network::HeaderProvider provider;
      if (!cookie_header.empty()) {
        provider = [cookie = std::string(cookie_header)](const url::Url&) {
          return std::vector<network::HttpHeader>{{"cookie", cookie}};
        };
      }
      return network::HttpGet(u, 5, provider);
    };
  }
}

BrowserController::~BrowserController()
{
  (void)Save();
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

int BrowserController::NewTab(const std::string& url, bool activate)
{
  int id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto tab = std::make_unique<Tab>();
    tab->id = next_tab_id_++;
    tabs_.push_back(std::move(tab));
    id = tabs_.back()->id;
    if (activate) {
      active_tab_ = static_cast<int>(tabs_.size()) - 1;
    }
  }
  if (!url.empty())
    (void)Navigate(id, url);
  return id;
}

void BrowserController::ActivateTab(int id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i]->id == id) {
      active_tab_ = static_cast<int>(i);
      return;
    }
  }
}

void BrowserController::CloseTab(int id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it =
      std::find_if(tabs_.begin(), tabs_.end(), [id](const auto& t) { return t->id == id; });
  if (it == tabs_.end())
    return;
  const size_t index = static_cast<size_t>(it - tabs_.begin());
  tabs_.erase(it);
  if (tabs_.empty()) {
    active_tab_ = -1;
    return;
  }
  if (active_tab_ >= static_cast<int>(tabs_.size()))
    active_tab_--;
  // Keep the tab at (or after) the closed one active.
  if (static_cast<int>(index) < active_tab_) {
    // active index unchanged
  } else if (static_cast<int>(index) == active_tab_) {
    active_tab_ = std::min(static_cast<int>(index), static_cast<int>(tabs_.size()) - 1);
  }
}

int BrowserController::active_tab() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return active_tab_;
}

Tab* BrowserController::ActiveTab()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size()))
    return nullptr;
  return tabs_[static_cast<size_t>(active_tab_)].get();
}

Tab* BrowserController::FindTab(int id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it =
      std::find_if(tabs_.begin(), tabs_.end(), [id](const auto& t) { return t->id == id; });
  return it == tabs_.end() ? nullptr : it->get();
}

ContentType BrowserController::active_content_type() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size())) {
    return ContentType::kError;
  }
  return tabs_[static_cast<size_t>(active_tab_)]->content_type;
}

// ---------------------------------------------------------------------------
// GUI snapshots
// ---------------------------------------------------------------------------

std::vector<TabSnapshot> BrowserController::SnapshotTabs() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TabSnapshot> out;
  out.reserve(tabs_.size());
  for (const auto& tab : tabs_) {
    out.push_back(ToSnapshot(*tab));
  }
  return out;
}

TabSnapshot BrowserController::SnapshotTab(int id) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& tab : tabs_) {
    if (tab->id == id)
      return ToSnapshot(*tab);
  }
  return TabSnapshot{};
}

TabSnapshot BrowserController::SnapshotActiveTab() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size())) {
    return TabSnapshot{};
  }
  return ToSnapshot(*tabs_[static_cast<size_t>(active_tab_)]);
}

std::vector<storage::HistoryEntry> BrowserController::SnapshotHistory() const
{
  return history_.All();
}

std::vector<storage::Bookmark> BrowserController::SnapshotBookmarks() const
{
  return bookmarks_.All();
}

std::vector<Download> BrowserController::SnapshotDownloads() const
{
  return downloads_.items();
}

size_t BrowserController::SnapshotCookieCount() const
{
  return cookies_.size();
}

std::vector<NetworkLogEntry> BrowserController::SnapshotNetworkLog() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return network_log_;
}

std::vector<ConsoleEntry> BrowserController::SnapshotConsoleLog() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return console_log_;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

std::string BrowserController::ResolveInput(const std::string& input) const
{
  std::string s = std::string(Trim(input));
  if (s.empty())
    return {};
  // Absolute URL already?
  auto parsed = url::Url::Parse(s);
  if (parsed.has_value())
    return s;
  // Bare hostname like "example.com" -> http://example.com
  if (s.find(' ') == std::string::npos && s.find('/') == std::string::npos &&
      s.find('.') != std::string::npos) {
    return "http://" + s;
  }
  // Treat as a local path.
  return s;
}

void BrowserController::NavigateToUrl(Tab& tab, const std::string& url_string)
{
  // Local paths (and file:// URLs) are handled directly without the URL
  // parser (which does not support opaque file: URLs yet).
  if (StartsWith(url_string, "file://")) {
    std::string path = url_string.substr(7);
    if (!path.empty() && path[0] == '/')
      path.erase(path.begin());
    LoadLocalPath(tab, path);
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
  FetchAndLoad(tab, parsed.value());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.loading = false;
  }
}

void BrowserController::LoadLocalPath(Tab& tab, const std::string& path)
{
  auto maybe_bytes = storage::ReadFile(path);
  if (!maybe_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.content_type = ContentType::kError;
    tab.error = std::make_shared<std::string>("cannot read file: " + maybe_bytes.error().message());
    tab.title = "File error";
    return;
  }
  LoadBytes(tab, maybe_bytes.value(), "", "file://" + path);
}

base::Result<void> BrowserController::Navigate(int tab_id, const std::string& input)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr)
    return base::Err(base::Error::InvalidArgument("no such tab"));
  const std::string target = ResolveInput(input);
  if (target.empty())
    return base::Err(base::Error::InvalidArgument("empty URL"));

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

  NavigateToUrl(*tab, target);

  // Update the address-bar URL for non-file schemes.
  if (!StartsWith(target, "file://")) {
    std::lock_guard<std::mutex> lock(mutex_);
    tab->url = target;
  }
  return base::Ok();
}

base::Result<void> BrowserController::NavigateActive(const std::string& input)
{
  Tab* tab = ActiveTab();
  if (tab == nullptr)
    return base::Err(base::Error::InvalidArgument("no active tab"));
  return Navigate(tab->id, input);
}

void BrowserController::Back()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr || !tab->CanGoBack())
    return;
  --tab->history_index;
  const std::string target = tab->history[static_cast<size_t>(tab->history_index)];
  NavigateToUrl(*tab, target);
  std::lock_guard<std::mutex> lock(mutex_);
  tab->url = target;
}

void BrowserController::Forward()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr || !tab->CanGoForward())
    return;
  ++tab->history_index;
  const std::string target = tab->history[static_cast<size_t>(tab->history_index)];
  NavigateToUrl(*tab, target);
  std::lock_guard<std::mutex> lock(mutex_);
  tab->url = target;
}

void BrowserController::Reload()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr || tab->url.empty())
    return;
  NavigateToUrl(*tab, tab->url);
}

void BrowserController::PumpScriptTimers()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr || tab->script_runtime == nullptr) {
    return;
  }
  if (tab->script_runtime->RunPendingTimers() > 0) {
    // Timers may have mutated the DOM; re-run the cascade so the next
    // Layout/Rasterize reflects the new state.
    if (tab->page != nullptr) {
      tab->page->ReapplyStyles();
    }
  }
}

// ---------------------------------------------------------------------------
// Fetch + content routing
// ---------------------------------------------------------------------------

void BrowserController::FetchAndLoad(Tab& tab, const url::Url& url)
{
  const auto start = std::chrono::steady_clock::now();
  const int64_t now = NowUnix();
  const std::string cookie = CookieHeader(url, now);
  auto response = fetch_(url, cookie);
  const auto end = std::chrono::steady_clock::now();
  const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

  NetworkLogEntry entry;
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

  const std::string content_type = response.value().GetHeader("content-type");
  LoadBytes(tab, response.value().body, content_type, url.Serialize());
}

void BrowserController::LoadBytes(Tab& tab,
                                  std::string_view bytes,
                                  std::string_view content_type,
                                  const std::string& final_url)
{
  const std::string ct = ToLower(content_type);

  const bool is_image = ct.find("image/") != std::string::npos || neko::image::IsPng(bytes) ||
                        neko::image::IsJpeg(bytes);
  const bool is_pdf = ct.find("pdf") != std::string::npos || neko::pdf::IsPdf(bytes);
  const bool is_audio = ct.find("audio/") != std::string::npos || neko::media::IsWav(bytes);
  const bool is_text = ct.find("text/") != std::string::npos;
  const bool is_html = ct.find("html") != std::string::npos ||
                       ct.find("xml") != std::string::npos ||
                       (ct.empty() && !is_image && !is_pdf && !is_audio && LooksLikeHtml(bytes));

  if (is_html) {
    // Build the new page entirely on the worker thread and publish it in one
    // atomic step: a published Page (and its payload) is never mutated by the
    // worker afterwards, so the GUI can safely hold a shared handle and
    // Layout/Rasterize/hit-test it at any time.
    auto new_page = std::make_shared<renderer::Page>();
    auto r = new_page->LoadHtml(bytes);
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
    // goes to DevTools' console log.
    tab.script_runtime = RunPageScripts(
        *new_page,
        final_url,
        [this, &tab](const url::Url& script_url) {
          return fetch_(script_url, CookieHeader(script_url, NowUnix()));
        },
        [this](std::string_view level, std::string_view text) { LogConsole(level, text); });
    // Fetch and decode the page's <img> subresources before publishing.
    FetchPageImages(*new_page, final_url, fetch_);
    std::string title = new_page->document()->Title();
    if (title.empty())
      title = final_url;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kHtml;
      tab.page = std::move(new_page);
      tab.title = std::move(title);
    }
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_image) {
    auto decoded = neko::image::DecodeImage(bytes);
    if (!decoded) {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kError;
      tab.error = std::make_shared<std::string>("image decode error: " + decoded.error().message());
      tab.title = "Image error";
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kImage;
      tab.image = std::make_shared<image::Image>(std::move(decoded.value()));
      tab.title = final_url;
    }
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_pdf) {
    auto parsed = neko::pdf::ExtractText(bytes);
    if (!parsed) {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kError;
      tab.error = std::make_shared<std::string>("pdf error: " + parsed.error().message());
      tab.title = "PDF error";
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kPdf;
      tab.pdf = std::make_shared<pdf::PdfDocument>(std::move(parsed.value()));
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tab.content_type = ContentType::kText;
      tab.raw_text = std::make_shared<std::string>(bytes);
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
  if (!r1)
    return r1.error();
  if (!r2)
    return r2.error();
  if (!r3)
    return r3.error();
  return r4;
}

base::Result<void> BrowserController::Save()
{
  cookies_.PurgeExpired(NowUnix());
  auto r1 = cookies_.Save();
  auto r2 = history_.Save();
  auto r3 = bookmarks_.Save();
  auto r4 = local_storage_.Save();
  if (!r1)
    return r1.error();
  if (!r2)
    return r2.error();
  if (!r3)
    return r3.error();
  return r4;
}

// ---------------------------------------------------------------------------
// Worker-thread store operations
// ---------------------------------------------------------------------------

base::Result<Download> BrowserController::StartDownload(const url::Url& url,
                                                        std::string_view cookie_header)
{
  return downloads_.Start(url, cookie_header);
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
  ClearNetworkLog();
  (void)Save();
}

std::string BrowserController::CookieHeader(const url::Url& url, int64_t now) const
{
  return cookies_.CookieHeaderFor(url, now);
}

// ---------------------------------------------------------------------------
// DevTools output
// ---------------------------------------------------------------------------

std::string BrowserController::DumpDom() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size()))
    return "";
  const std::shared_ptr<renderer::Page>& page = tabs_[static_cast<size_t>(active_tab_)]->page;
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

void FetchPageImages(renderer::Page& page,
                     const std::string& base_url,
                     const BrowserController::FetchFn& fetch)
{
  dom::Document* doc = page.document();
  if (doc == nullptr) {
    return;
  }
  const base::Result<url::Url> base = url::Url::Parse(base_url);

  // Depth-first walk collecting <img src> subresources.
  std::vector<dom::Element*> images;
  std::vector<dom::Node*> stack;
  for (dom::Node* child : doc->ChildNodes()) {
    stack.push_back(child);
  }
  while (!stack.empty()) {
    dom::Node* node = stack.back();
    stack.pop_back();
    if (node->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element* element = static_cast<dom::Element*>(node);
    if (element->tag_name() == "img") {
      images.push_back(element);
    }
    for (dom::Node* child : node->ChildNodes()) {
      stack.push_back(child);
    }
  }

  // Phase: fetch every subresource on the calling thread (the FetchFn may be
  // stateful and is not guaranteed thread-safe), then decode the bodies in
  // parallel on a thread pool, then inject the decoded images back on the
  // calling thread (SetElementImage locks internally).
  struct PendingImage
  {
    dom::Element* element = nullptr;
    std::string url;
    std::string bytes;
  };
  std::vector<PendingImage> pending;
  pending.reserve(images.size());
  for (dom::Element* element : images) {
    const std::optional<std::string_view> src = element->GetAttribute("src");
    if (!src.has_value() || src->empty()) {
      continue;
    }
    base::Result<url::Url> target = url::Url::Parse(*src);
    if (base.has_value()) {
      target = url::Url::Parse(*src, base.value());
    }
    if (!target.has_value()) {
      NEKO_LOG_WARNING("img: cannot resolve src \"" + std::string(*src) + "\"");
      continue;
    }
    const auto response = fetch(target.value(), {});
    if (!response) {
      NEKO_LOG_WARNING("img: fetch failed for " + target.value().Serialize());
      continue;
    }
    pending.push_back(PendingImage{element, target.value().Serialize(), response.value().body});
  }
  if (pending.empty()) {
    return;
  }

  if (pending.size() == 1) {
    // A single image: decode inline (no thread-pool overhead).
    auto decoded = image::DecodeImage(pending[0].bytes);
    if (!decoded) {
      NEKO_LOG_WARNING("img: decode failed for " + pending[0].url);
      return;
    }
    page.SetElementImage(*pending[0].element, std::move(decoded.value()));
    NEKO_LOG_INFO("img: injected " + pending[0].url);
    return;
  }

  base::ThreadPool pool;
  std::vector<std::future<base::Result<image::Image>>> futures;
  futures.reserve(pending.size());
  for (PendingImage& item : pending) {
    futures.push_back(
        pool.Submit([bytes = std::move(item.bytes)]() { return image::DecodeImage(bytes); }));
  }
  for (std::size_t i = 0; i < pending.size(); ++i) {
    auto decoded = futures[i].get();
    if (!decoded) {
      NEKO_LOG_WARNING("img: decode failed for " + pending[i].url);
      continue;
    }
    page.SetElementImage(*pending[i].element, std::move(decoded.value()));
    NEKO_LOG_INFO("img: injected " + pending[i].url);
  }
}

} // namespace neko::browser
