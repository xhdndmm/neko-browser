#include "neko/browser/browser_controller.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>

#include "neko/base/logging.h"
#include "neko/network/http.h"
#include "neko/storage/file_util.h"

namespace neko::browser {
namespace {

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

int64_t NowUnix() { return static_cast<int64_t>(std::time(nullptr)); }

// Lowercases an ASCII string.
std::string ToLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(
        c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  return out;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// True when |bytes| begin with '<' (after optional whitespace/BOM), which we
// take as a hint for HTML content when no Content-Type was provided.
bool LooksLikeHtml(std::string_view bytes) {
  size_t i = 0;
  if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF &&
      static_cast<uint8_t>(bytes[1]) == 0xBB && static_cast<uint8_t>(bytes[2]) == 0xBF) {
    i = 3;
  }
  while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\n' ||
                              bytes[i] == '\r')) {
    ++i;
  }
  return i < bytes.size() && bytes[i] == '<';
}

}  // namespace

std::string_view ToString(ContentType type) {
  switch (type) {
    case ContentType::kHtml: return "html";
    case ContentType::kImage: return "image";
    case ContentType::kPdf: return "pdf";
    case ContentType::kAudio: return "audio";
    case ContentType::kText: return "text";
    case ContentType::kOther: return "other";
    case ContentType::kError: return "error";
  }
  return "unknown";
}

BrowserController::BrowserController(std::string profile_dir, FetchFn fetch)
    : profile_dir_(std::move(profile_dir)),
      fetch_(std::move(fetch)),
      cookies_(profile_dir_),
      history_(profile_dir_),
      bookmarks_(profile_dir_),
      downloads_(profile_dir_ + "/downloads") {
  // Default fetch: network::HttpGet with the controller-provided cookie
  // header.
  if (!fetch_) {
    fetch_ = [](const url::Url& u, std::string_view cookie_header) {
      network::HeaderProvider provider;
      if (!cookie_header.empty()) {
        provider = [cookie = std::string(cookie_header)](const url::Url&) {
          return std::vector<network::HttpHeader>{{ "cookie", cookie }};
        };
      }
      return network::HttpGet(u, 5, provider);
    };
  }
}

BrowserController::~BrowserController() { (void)Save(); }

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

int BrowserController::NewTab(const std::string& url, bool activate) {
  auto tab = std::make_unique<Tab>();
  tab->id = next_tab_id_++;
  tabs_.push_back(std::move(tab));
  const int id = tabs_.back()->id;
  if (activate) {
    ActivateTab(id);
    if (!url.empty()) (void)Navigate(id, url);
  } else if (!url.empty()) {
    (void)Navigate(id, url);
  }
  return id;
}

void BrowserController::ActivateTab(int id) {
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i]->id == id) {
      active_tab_ = static_cast<int>(i);
      return;
    }
  }
}

void BrowserController::CloseTab(int id) {
  const auto it = std::find_if(tabs_.begin(), tabs_.end(),
                               [id](const auto& t) { return t->id == id; });
  if (it == tabs_.end()) return;
  const size_t index = static_cast<size_t>(it - tabs_.begin());
  tabs_.erase(it);
  if (tabs_.empty()) {
    active_tab_ = -1;
    return;
  }
  if (active_tab_ >= static_cast<int>(tabs_.size())) active_tab_--;
  // Keep the tab at (or after) the closed one active.
  if (static_cast<int>(index) < active_tab_) {
    // active index unchanged
  } else if (static_cast<int>(index) == active_tab_) {
    active_tab_ = std::min(static_cast<int>(index), static_cast<int>(tabs_.size()) - 1);
  }
}

Tab* BrowserController::ActiveTab() {
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size())) return nullptr;
  return tabs_[static_cast<size_t>(active_tab_)].get();
}

Tab* BrowserController::FindTab(int id) {
  const auto it = std::find_if(tabs_.begin(), tabs_.end(),
                               [id](const auto& t) { return t->id == id; });
  return it == tabs_.end() ? nullptr : it->get();
}

ContentType BrowserController::active_content_type() const {
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size())) {
    return ContentType::kError;
  }
  return tabs_[static_cast<size_t>(active_tab_)]->content_type;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

std::string BrowserController::ResolveInput(const std::string& input) const {
  std::string s = std::string(Trim(input));
  if (s.empty()) return {};
  // Absolute URL already?
  auto parsed = url::Url::Parse(s);
  if (parsed.has_value()) return s;
  // Bare hostname like "example.com" -> http://example.com
  if (s.find(' ') == std::string::npos && s.find('/') == std::string::npos &&
      s.find('.') != std::string::npos) {
    return "http://" + s;
  }
  // Treat as a local path.
  return s;
}

void BrowserController::NavigateToUrl(Tab& tab, const std::string& url_string) {
  // Local paths (and file:// URLs) are handled directly without the URL
  // parser (which does not support opaque file: URLs yet).
  if (StartsWith(url_string, "file://")) {
    std::string path = url_string.substr(7);
    if (!path.empty() && path[0] == '/') path.erase(path.begin());
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
  tab.loading = true;
  FetchAndLoad(tab, parsed.value());
  tab.loading = false;
}

void BrowserController::LoadLocalPath(Tab& tab, const std::string& path) {
  auto maybe_bytes = storage::ReadFile(path);
  if (!maybe_bytes) {
    tab.content_type = ContentType::kError;
    tab.error = "cannot read file: " + maybe_bytes.error().message();
    tab.title = "File error";
    return;
  }
  LoadBytes(tab, maybe_bytes.value(), "", "file://" + path);
}

base::Result<void> BrowserController::Navigate(int tab_id, const std::string& input) {
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) return base::Err(base::Error::InvalidArgument("no such tab"));
  const std::string target = ResolveInput(input);
  if (target.empty()) return base::Err(base::Error::InvalidArgument("empty URL"));

  // Push onto the back/forward stack (truncating any forward entries).
  if (!tab->history.empty() && tab->history_index >= 0 &&
      tab->history[static_cast<size_t>(tab->history_index)] == target) {
    // Reload of the current entry.
  } else {
    if (tab->history_index >= 0 &&
        tab->history_index + 1 < static_cast<int>(tab->history.size())) {
      tab->history.resize(static_cast<size_t>(tab->history_index) + 1);
    }
    tab->history.push_back(target);
    tab->history_index = static_cast<int>(tab->history.size()) - 1;
  }

  NavigateToUrl(*tab, target);

  // Update the address-bar URL for non-file schemes.
  if (!StartsWith(target, "file://")) {
    tab->url = target;
  }
  return base::Ok();
}

base::Result<void> BrowserController::NavigateActive(const std::string& input) {
  Tab* tab = ActiveTab();
  if (tab == nullptr) return base::Err(base::Error::InvalidArgument("no active tab"));
  return Navigate(tab->id, input);
}

void BrowserController::Back() {
  Tab* tab = ActiveTab();
  if (tab == nullptr || !tab->CanGoBack()) return;
  --tab->history_index;
  const std::string target = tab->history[static_cast<size_t>(tab->history_index)];
  NavigateToUrl(*tab, target);
  tab->url = target;
}

void BrowserController::Forward() {
  Tab* tab = ActiveTab();
  if (tab == nullptr || !tab->CanGoForward()) return;
  ++tab->history_index;
  const std::string target = tab->history[static_cast<size_t>(tab->history_index)];
  NavigateToUrl(*tab, target);
  tab->url = target;
}

void BrowserController::Reload() {
  Tab* tab = ActiveTab();
  if (tab == nullptr || tab->url.empty()) return;
  NavigateToUrl(*tab, tab->url);
}

// ---------------------------------------------------------------------------
// Fetch + content routing
// ---------------------------------------------------------------------------

void BrowserController::FetchAndLoad(Tab& tab, const url::Url& url) {
  const auto start = std::chrono::steady_clock::now();
  const int64_t now = NowUnix();
  const std::string cookie = cookies_.CookieHeaderFor(url, now);
  auto response = fetch_(url, cookie);
  const auto end = std::chrono::steady_clock::now();
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(end - start).count();

  NetworkLogEntry entry;
  entry.url = url.Serialize();
  entry.elapsed_ms = elapsed_ms;
  entry.timestamp = NowUnix();
  if (!response) {
    entry.error = response.error().message();
    network_log_.push_back(entry);
    tab.content_type = ContentType::kError;
    tab.error = "network error: " + response.error().message();
    tab.title = "Network error";
    LogConsole("error", "failed to fetch " + url.Serialize() + ": " +
                            response.error().message());
    return;
  }

  entry.status = response.value().status_code;
  entry.bytes = static_cast<int64_t>(response.value().body.size());
  network_log_.push_back(entry);

  // Consume Set-Cookie headers.
  for (const network::HttpHeader& header : response.value().headers) {
    if (header.name == "set-cookie") {
      cookies_.SetCookieFromHeader(url, header.value, NowUnix());
    }
  }

  if (response.value().status_code >= 400) {
    tab.content_type = ContentType::kError;
    tab.error = "HTTP " + std::to_string(response.value().status_code) + " " +
                response.value().reason;
    tab.title = "HTTP " + std::to_string(response.value().status_code);
    LogConsole("warning", tab.error);
    return;
  }

  const std::string content_type = response.value().GetHeader("content-type");
  LoadBytes(tab, response.value().body, content_type, url.Serialize());
}

void BrowserController::LoadBytes(Tab& tab, std::string_view bytes,
                                  std::string_view content_type,
                                  const std::string& final_url) {
  const std::string ct = ToLower(content_type);

  const bool is_image = ct.find("image/") != std::string::npos ||
                        neko::image::IsPng(bytes) || neko::image::IsJpeg(bytes);
  const bool is_pdf =
      ct.find("pdf") != std::string::npos || neko::pdf::IsPdf(bytes);
  const bool is_audio =
      ct.find("audio/") != std::string::npos || neko::media::IsWav(bytes);
  const bool is_text = ct.find("text/") != std::string::npos;
  const bool is_html = ct.find("html") != std::string::npos ||
                       ct.find("xml") != std::string::npos ||
                       (ct.empty() && !is_image && !is_pdf && !is_audio &&
                        LooksLikeHtml(bytes));

  if (is_html) {
    tab.content_type = ContentType::kHtml;
    auto r = tab.page.LoadHtml(bytes);
    if (!r) {
      tab.content_type = ContentType::kError;
      tab.error = "HTML parse error: " + r.error().message();
      tab.title = "Parse error";
      return;
    }
    tab.title = tab.page.document()->Title();
    if (tab.title.empty()) tab.title = final_url;
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_image) {
    auto decoded = neko::image::DecodeImage(bytes);
    if (!decoded) {
      tab.content_type = ContentType::kError;
      tab.error = "image decode error: " + decoded.error().message();
      tab.title = "Image error";
      return;
    }
    tab.content_type = ContentType::kImage;
    tab.image = std::move(decoded.value());
    tab.title = final_url;
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_pdf) {
    auto parsed = neko::pdf::ExtractText(bytes);
    if (!parsed) {
      tab.content_type = ContentType::kError;
      tab.error = "pdf error: " + parsed.error().message();
      tab.title = "PDF error";
      return;
    }
    tab.content_type = ContentType::kPdf;
    tab.pdf = std::move(parsed.value());
    tab.title = tab.pdf.title.empty() ? final_url : tab.pdf.title;
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_audio) {
    auto decoded = neko::media::DecodeWav(bytes);
    if (!decoded) {
      tab.content_type = ContentType::kError;
      tab.error = "audio decode error: " + decoded.error().message();
      tab.title = "Audio error";
      return;
    }
    tab.content_type = ContentType::kAudio;
    tab.audio = std::move(decoded.value());
    tab.title = final_url;
    RecordVisit(final_url, tab.title);
    return;
  }

  if (is_text) {
    tab.content_type = ContentType::kText;
    tab.raw_text.assign(bytes);
    tab.title = final_url;
    RecordVisit(final_url, tab.title);
    return;
  }

  // Unknown binary content.
  tab.content_type = ContentType::kOther;
  tab.raw_text.assign(bytes);
  tab.title = final_url;
  RecordVisit(final_url, tab.title);
}

void BrowserController::RecordVisit(const std::string& url, const std::string& title) {
  history_.RecordVisit(url, title, NowUnix());
}

// ---------------------------------------------------------------------------
// Bookmarks / console
// ---------------------------------------------------------------------------

base::Result<std::string> BrowserController::BookmarkActive(const std::string& folder) {
  Tab* tab = ActiveTab();
  if (tab == nullptr || tab->url.empty()) {
    return base::Err(base::Error::InvalidArgument("nothing to bookmark"));
  }
  return bookmarks_.Add(tab->url, tab->title.empty() ? tab->url : tab->title, folder,
                        NowUnix());
}

void BrowserController::LogConsole(std::string_view level, std::string_view message) {
  console_log_.push_back(ConsoleEntry{std::string(level), std::string(message),
                                      NowUnix()});
  if (console_log_.size() > 500) {
    console_log_.erase(console_log_.begin(), console_log_.begin() +
                                                 static_cast<ptrdiff_t>(console_log_.size() - 500));
  }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

base::Result<void> BrowserController::Load() {
  auto r1 = cookies_.Load();
  auto r2 = history_.Load();
  auto r3 = bookmarks_.Load();
  if (!r1) return r1.error();
  if (!r2) return r2.error();
  return r3;
}

base::Result<void> BrowserController::Save() {
  cookies_.PurgeExpired(NowUnix());
  auto r1 = cookies_.Save();
  auto r2 = history_.Save();
  auto r3 = bookmarks_.Save();
  if (!r1) return r1.error();
  if (!r2) return r2.error();
  return r3;
}

// ---------------------------------------------------------------------------
// DevTools output
// ---------------------------------------------------------------------------

std::string BrowserController::DumpDom() const {
  if (active_tab_ < 0 || active_tab_ >= static_cast<int>(tabs_.size())) return "";
  return tabs_[static_cast<size_t>(active_tab_)]->page.DumpDom();
}

std::string BrowserController::DumpNetworkLog() const {
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

}  // namespace neko::browser
