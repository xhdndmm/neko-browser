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
#include "neko/image/image.h"
#include "neko/network/http.h"
#include "neko/paint/rasterizer.h"
#include "neko/security/origin.h"
#include "neko/storage/file_util.h"
#include "neko/url/url.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

void RemoveLastUtf8CodePoint(std::string& value)
{
  if (value.empty()) {
    return;
  }
  value.pop_back();
  bool removed_continuation = false;
  while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xc0U) == 0x80U) {
    value.pop_back();
    removed_continuation = true;
  }
  if (removed_continuation && !value.empty()) {
    value.pop_back();
  }
}

// Resolves a possibly-relative reference against a base page URL.  Handles
// file:// / bare local path bases (which the URL parser rejects) by string
// concatenation, and leaves absolute references (those carrying a scheme)
// unchanged — see browser::ResolveReference.
std::string ResolveUrlAgainstBase(std::string_view ref, std::string_view base_url)
{
  if (ref.empty()) {
    return std::string(base_url);
  }
  const std::optional<std::string> resolved = ResolveReference(ref, base_url);
  return resolved.has_value() ? resolved.value() : std::string(ref);
}

// Collects the named, enabled form controls of |form| and encodes them as an
// application/x-www-form-urlencoded string (HTML §4.10.21.5, simplified: text
// /search /hidden /password inputs, checkboxes and radios, textarea, first
// option of a select).
std::string CollectFormData(const dom::Element& form)
{
  std::string out;
  const auto add_field = [&](const std::string& name, const std::string& value) {
    if (name.empty()) {
      return;
    }
    if (!out.empty()) {
      out += '&';
    }
    out += url::PercentEncode(name) + '=' + url::PercentEncode(value);
  };
  std::vector<dom::Node*> stack;
  // Pre-order (document-order) walk: push children in reverse so they pop in
  // tree order, matching the HTML entry-list order.
  auto push_children = [&stack](dom::Node& n) {
    std::vector<dom::Node*> children;
    for (dom::Node* c : n.ChildNodes()) {
      children.push_back(c);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(*it);
    }
  };
  push_children(const_cast<dom::Element&>(form));
  while (!stack.empty()) {
    dom::Node* n = stack.back();
    stack.pop_back();
    if (n->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element& e = static_cast<dom::Element&>(*n);
    push_children(e);
    const std::string tag = std::string(e.tag_name());
    if (tag != "input" && tag != "textarea" && tag != "select") {
      continue;
    }
    const std::optional<std::string_view> name = e.GetAttribute("name");
    if (!name.has_value() || name->empty() || e.HasAttribute("disabled")) {
      continue;
    }
    if (tag == "textarea") {
      add_field(std::string(*name), e.TextContent());
    } else if (tag == "select") {
      for (dom::Node* c : e.ChildNodes()) {
        if (c->node_type() != dom::NodeType::kElement) {
          continue;
        }
        dom::Element& opt = static_cast<dom::Element&>(*c);
        if (opt.tag_name() == "option") {
          const std::optional<std::string_view> val = opt.GetAttribute("value");
          add_field(std::string(*name), val.has_value() ? std::string(*val) : opt.TextContent());
          break;
        }
      }
    } else {
      const std::string type = std::string(e.GetAttribute("type").value_or("text"));
      if (type == "checkbox" || type == "radio") {
        if (!e.HasAttribute("checked")) {
          continue;
        }
        const std::optional<std::string_view> val = e.GetAttribute("value");
        add_field(std::string(*name), val.has_value() ? std::string(*val) : "on");
      } else if (type == "hidden" || type == "text" || type == "search" || type == "password" ||
                 type == "email" || type == "url") {
        const std::optional<std::string_view> val = e.GetAttribute("value");
        add_field(std::string(*name), val.has_value() ? std::string(*val) : "");
      }
      // submit/reset/button/file/image/... are not part of the entry list here.
    }
  }
  return out;
}

// Walks from |node| up to find a submit button (<button> or <input
// type=submit>), then returns the <form> that owns it.
dom::Element* FindSubmitForm(dom::Node* node)
{
  dom::Element* button = nullptr;
  for (dom::Node* n = node; n != nullptr; n = n->parent()) {
    if (n->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element& e = static_cast<dom::Element&>(*n);
    const std::string tag = std::string(e.tag_name());
    if (tag == "button") {
      const std::string type = std::string(e.GetAttribute("type").value_or("submit"));
      if (type == "submit") {
        button = &e;
        break;
      }
    } else if (tag == "input") {
      const std::string type = std::string(e.GetAttribute("type").value_or("text"));
      if (type == "submit") {
        button = &e;
        break;
      }
    }
  }
  if (button == nullptr) {
    return nullptr;
  }
  for (dom::Node* n = button; n != nullptr; n = n->parent()) {
    if (n->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element& e = static_cast<dom::Element&>(*n);
    if (e.tag_name() == "form") {
      return &e;
    }
  }
  return nullptr;
}

// Returns the closest <form> ancestor of a control, if any (used for
// implicit submission when Enter is pressed inside a text input).
dom::Element* FindEnclosingForm(dom::Node* node)
{
  for (dom::Node* n = node; n != nullptr; n = n->parent()) {
    if (n->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element& e = static_cast<dom::Element&>(*n);
    if (e.tag_name() == "form") {
      return &e;
    }
  }
  return nullptr;
}

int64_t NowUnix()
{
  return static_cast<int64_t>(std::time(nullptr));
}

// Monotonic milliseconds (frame-pull throttling).
int64_t NowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Viewport used for a renderer session until the GUI reports its own.
inline constexpr int kDefaultRemoteViewportWidth = 800;
inline constexpr int kDefaultRemoteViewportHeight = 600;
// Frames are pulled at most this often for scroll-driven repaints; anything
// faster is coalesced into the next pump.
inline constexpr int64_t kRemoteFrameMinIntervalMs = 40;

// True when |tab| renders its document in a renderer child (renderer-process
// mode showing HTML).  After navigating to non-HTML content or after a
// session failure the tab falls back to the in-process paths.
bool IsRemoteTab(const Tab& tab)
{
  return tab.session != nullptr && tab.page == nullptr && tab.content_type == ContentType::kHtml;
}

// The CLI binary serves --renderer-session and ships next to the GUI
// executable in the build/install layout.
std::string DefaultRendererExecutable()
{
  const std::string self = SelfExecutablePath();
  const std::size_t slash = self.find_last_of("/\\");
  if (self.empty() || slash == std::string::npos) {
    return {};
  }
#ifdef _WIN32
  const std::string candidate = self.substr(0, slash + 1) + "neko_browser.exe";
#else
  const std::string candidate = self.substr(0, slash + 1) + "neko_browser";
#endif
  std::error_code ec;
  if (std::filesystem::exists(candidate, ec) && !ec) {
    return candidate;
  }
  return {};
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

// ---------------------------------------------------------------------------
// Password-manager form inspection helpers.  The caller holds the page's DOM
// lock (or owns the document outright, as in LoadBytes before publication).
// ---------------------------------------------------------------------------

// The <input> elements under |root| in document order (pre-order walk).
std::vector<dom::Element*> CollectInputs(dom::Node& root)
{
  std::vector<dom::Element*> inputs;
  std::vector<dom::Node*> stack;
  auto push_children = [&stack](dom::Node& n) {
    std::vector<dom::Node*> children;
    for (dom::Node* c : n.ChildNodes()) {
      children.push_back(c);
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(*it);
    }
  };
  push_children(root);
  while (!stack.empty()) {
    dom::Node* n = stack.back();
    stack.pop_back();
    if (n->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element& e = static_cast<dom::Element&>(*n);
    push_children(e);
    if (e.tag_name() == "input") {
      inputs.push_back(&e);
    }
  }
  return inputs;
}

// Lowercased input type; a missing attribute means "text".
std::string InputType(const dom::Element& input)
{
  return ToLower(input.GetAttribute("type").value_or("text"));
}

// Text-entry controls that can hold a username.
bool IsUsernameInputType(std::string_view type)
{
  return type == "text" || type == "search" || type == "email" || type == "url" || type == "tel";
}

// The nearest preceding text-ish input of |password| within the same form
// (page-level controls count as one anonymous form).  |inputs| is the
// document-order input list |password| belongs to.
dom::Element* FindUsernameInput(const std::vector<dom::Element*>& inputs, dom::Element* password)
{
  const auto form_of = [](dom::Element* element) -> dom::Element* {
    for (dom::Node* n = element->parent(); n != nullptr; n = n->parent()) {
      if (n->node_type() != dom::NodeType::kElement) {
        continue;
      }
      dom::Element& e = static_cast<dom::Element&>(*n);
      if (e.tag_name() == "form") {
        return &e;
      }
    }
    return nullptr;
  };
  dom::Element* password_form = form_of(password);
  dom::Element* best = nullptr;
  for (dom::Element* input : inputs) {
    if (input == password) {
      break;
    }
    if (form_of(input) != password_form) {
      continue;
    }
    if (IsUsernameInputType(InputType(*input))) {
      best = input;
    }
  }
  return best;
}

// Guards the synchronous navigation chain against runaway JS-driven loops (a
// page that keeps assigning window.location, or two pages that redirect to
// each other).  Each hop is a real network fetch; the cap mirrors the HTTP
// redirect limit.  Thread-local because each worker uses its own chain.
class NavigationDepthGuard
{
public:
  explicit NavigationDepthGuard(int limit) : limit_(limit)
  {
    ++depth_;
  }
  ~NavigationDepthGuard()
  {
    --depth_;
  }
  bool Exceeded() const
  {
    return depth_ > limit_;
  }
  NavigationDepthGuard(const NavigationDepthGuard&) = delete;
  NavigationDepthGuard& operator=(const NavigationDepthGuard&) = delete;

private:
  int limit_;
  static thread_local int depth_;
};

thread_local int NavigationDepthGuard::depth_ = 0;

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
  s.origin = tab.origin;
  s.page = tab.page;
  s.image = tab.image;
  s.image_frame = tab.image_frame;
  s.pdf = tab.pdf;
  s.audio = tab.audio;
  s.raw_text = tab.raw_text;
  s.error = tab.error;
  s.scroll_request_id = tab.scroll_request_id;
  s.pending_scroll_y = tab.pending_scroll_y;
  // "Remote" describes the CURRENT document: a tab whose site session is alive
  // but that currently shows an image / text / error page renders locally.
  s.remote = IsRemoteTab(tab);
  s.remote_frame = tab.remote_frame;
  s.remote_content_height = tab.remote_content_height;
  s.remote_hover_link = tab.remote_hover_link;
  // In-process frames (ADR 0020) and child-process frames share one snapshot
  // shape: the GUI draws |frame| and never touches the live page.
  s.frame = s.remote ? tab.remote_frame : tab.frame;
  s.frame_content_height = s.remote ? tab.remote_content_height : tab.frame_content_height;
  s.frame_hover_link = s.remote ? tab.remote_hover_link : tab.frame_hover_link;
  if (!s.remote) {
    s.has_caret = tab.has_caret;
    s.caret_x = tab.caret_x;
    s.caret_y = tab.caret_y;
    s.caret_height = tab.caret_height;
  }
  s.zoom = tab.zoom;
  s.find_query = tab.find_query;
  s.find_match_count = tab.find_match_count;
  s.find_current_index = tab.find_index;
  s.find_current_match = tab.find_match;
  if (tab.pending_credential.has_value()) {
    s.has_pending_credential = true;
    s.pending_credential_origin = tab.pending_credential->origin;
    s.pending_credential_username = tab.pending_credential->username;
  }
  return s;
}

// Returns the allocator's free pages to the OS when the enclosing operation
// ends (see base::ReleaseFreeMemory).  Wrapped around operations that replace
// or drop a tab's document: without it the previous DOM, style store and
// images stay resident in the glibc arenas even though they were already
// freed (measured in the GUI: a closed tab released ~1 MiB of its ~126 MiB).
class HeapTrimOnExit
{
public:
  HeapTrimOnExit() = default;
  HeapTrimOnExit(const HeapTrimOnExit&) = delete;
  HeapTrimOnExit& operator=(const HeapTrimOnExit&) = delete;
  ~HeapTrimOnExit()
  {
    base::ReleaseFreeMemory();
  }
};

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
  // Default fetch: compute cookies for each redirect hop from the controller's
  // cookie jar.  HttpGet invokes HeaderProvider with the current hop URL.
  // With network-process mode the whole fetch runs in a child; the child asks
  // back for each hop's cookies, so the jar (and HttpOnly values) stay here.
  if (!fetch_) {
    fetch_ = [this](const url::Url& u, std::string_view) {
      if (network_.enabled) {
        NetworkSession* session = EnsureNetworkSession();
        if (session != nullptr) {
          NetworkSession::CookieLookupFn lookup = [this](const url::Url& target) {
            return CookieHeader(target, NowUnix());
          };
          auto fetched = session->Fetch(u, /*cookie_header=*/{}, lookup);
          if (fetched.has_value()) {
            return fetched;
          }
          NEKO_LOG_WARNING("network process fetch failed; falling back in-process: " +
                           fetched.error().message());
        }
      }
      network::HeaderProvider provider;
      provider = [this](const url::Url& target) {
        const std::string cookie = CookieHeader(target, NowUnix());
        if (cookie.empty()) {
          return std::vector<network::HttpHeader>{};
        }
        return std::vector<network::HttpHeader>{{"cookie", cookie}};
      };
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
  std::shared_ptr<RendererSession> retired;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it =
        std::find_if(tabs_.begin(), tabs_.end(), [id](const auto& t) { return t->id == id; });
    if (it == tabs_.end())
      return;
    const size_t index = static_cast<size_t>(it - tabs_.begin());
    // Take the renderer session out of the tab so the child is shut down
    // after the lock is released (its destructor reaps the process).
    retired = std::move((*it)->session);
    tabs_.erase(it);
    if (tabs_.empty()) {
      active_tab_ = -1;
    } else {
      if (active_tab_ >= static_cast<int>(tabs_.size()))
        active_tab_--;
      // Keep the tab at (or after) the closed one active.
      if (static_cast<int>(index) == active_tab_) {
        active_tab_ = std::min(static_cast<int>(index), static_cast<int>(tabs_.size()) - 1);
      }
    }
  }
  retired.reset();
  // The closed tab's document (DOM, style store, images, frame buffers) is
  // destroyed just above; hand its arena pages back to the OS so closing a
  // tab actually lowers the process footprint (see base::ReleaseFreeMemory).
  base::ReleaseFreeMemory();
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

std::vector<storage::Cookie> BrowserController::SnapshotCookies() const
{
  return cookies_.All();
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

void BrowserController::NavigateToUrl(Tab& tab, const std::string& url_string)
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tab.url = url_string;
  }
  // A fresh document replaces the old one; any focused element is stale.
  tab.focused_element = nullptr;
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
  if (StartsWith(url_string, "file://")) {
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
  FetchAndLoad(tab, parsed.value());
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
  // Focus a clicked form control so keyboard input reaches it; clicking
  // anywhere else clears the focus.  The page keeps the focused element for
  // the UI's caret painting (read across threads under the page lock).
  // Focus changes fire blur (old) / focus (new) on the elements.
  const std::string clicked_tag = std::string(element->tag_name());
  dom::Element* prev_focused = tab->focused_element;
  if (clicked_tag == "input" || clicked_tag == "textarea" || clicked_tag == "select") {
    tab->focused_element = const_cast<dom::Element*>(element);
    tab->page->SetFocusedElement(element);
    if (tab->script_runtime != nullptr && prev_focused != tab->focused_element) {
      if (prev_focused != nullptr) {
        tab->script_runtime->DispatchFocusEvent(*prev_focused, "blur");
      }
      tab->script_runtime->DispatchFocusEvent(*tab->focused_element, "focus");
    }
  } else if (tab->focused_element != nullptr) {
    dom::Element* losing = tab->focused_element;
    tab->focused_element = nullptr;
    tab->page->SetFocusedElement(nullptr);
    if (tab->script_runtime != nullptr) {
      tab->script_runtime->DispatchFocusEvent(*losing, "blur");
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
    const bool style_changed = tab->script_runtime->TakeStyleDirty();
    const bool dom_changed = tab->script_runtime->TakeDomDirty();
    if (style_changed) {
      tab->page->ReapplyStyles();
    }
    if (style_changed || dom_changed) {
      SchedulePendingImageFetch(*tab);
    }
    if (!not_canceled) {
      return true; // preventDefault: the page handled the click.
    }
  }
  // Default action: navigate an <a href> hyperlink (or a clickable element
  // nested inside one).
  const std::optional<std::string> target = HyperlinkTarget(element, tab->url);
  if (target.has_value()) {
    static_cast<void>(Navigate(tab_id, target.value()));
    return true;
  }
  // Default action: a submit button submits its enclosing form.
  if (dom::Element* form = FindSubmitForm(const_cast<dom::Element*>(element))) {
    SubmitForm(tab_id, form);
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
    const bool style_changed = tab->script_runtime->TakeStyleDirty();
    const bool dom_changed = tab->script_runtime->TakeDomDirty();
    if (style_changed) {
      tab->page->ReapplyStyles();
    }
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
  const bool wheel_style_changed = tab->script_runtime->TakeStyleDirty();
  const bool wheel_dom_changed = tab->script_runtime->TakeDomDirty();
  if (wheel_style_changed) {
    tab->page->ReapplyStyles();
  }
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
  if (!not_canceled || type != "keydown") {
    return not_canceled;
  }
  // Default actions: a printable character inserts into a focused text input,
  // Backspace deletes, Enter submits the enclosing form (implicit submission).
  dom::Element* control = tab->focused_element;
  const bool input_is_text = control != nullptr && control->tag_name() == "input";
  const bool textarea_is_text = control != nullptr && control->tag_name() == "textarea";
  bool value_changed = false;
  if (input_is_text) {
    const std::string input_type = std::string(control->GetAttribute("type").value_or("text"));
    const bool text_like = input_type == "text" || input_type == "search" ||
                           input_type == "password" || input_type == "email" ||
                           input_type == "url" || input_type == "hidden";
    if (text_like) {
      std::string value = std::string(control->GetAttribute("value").value_or(""));
      if (key == "Backspace") {
        RemoveLastUtf8CodePoint(value);
      } else if (key == "Enter") {
        if (dom::Element* form = FindEnclosingForm(control)) {
          SubmitForm(tab_id, form);
        }
        return not_canceled;
      } else if (!key.empty() && static_cast<unsigned char>(key.front()) >= 0x20U) {
        value += key;
      } else {
        return not_canceled;
      }
      control->SetAttribute("value", value);
      value_changed = true;
      // Fire the bubbling "input" event (oninput / addEventListener('input')).
      if (tab->script_runtime != nullptr) {
        tab->script_runtime->DispatchInputEvent(*control);
      }
    }
  } else if (textarea_is_text) {
    std::string value = control->TextContent();
    if (key == "Backspace") {
      RemoveLastUtf8CodePoint(value);
    } else if (key == "Enter") {
      value.push_back('\n');
    } else if (!key.empty() && static_cast<unsigned char>(key.front()) >= 0x20U) {
      value += key;
    } else {
      return not_canceled;
    }
    while (control->first_child() != nullptr) {
      control->RemoveChild(control->first_child());
    }
    control->AppendChild(std::make_unique<dom::Text>(std::move(value)));
    value_changed = true;
    if (tab->script_runtime != nullptr) {
      tab->script_runtime->DispatchInputEvent(*control);
    }
  }
  // Reflect DOM changes from the keydown/input handlers or the value edit
  // (the layout rebuilds and the UI repaints on the next StateChanged).  The
  // value edit always rebuilds, even on pages with no scripts.
  bool key_style_changed = value_changed;
  bool key_dom_changed = value_changed;
  if (!value_changed && tab->script_runtime != nullptr) {
    key_style_changed = tab->script_runtime->TakeStyleDirty();
    key_dom_changed = tab->script_runtime->TakeDomDirty();
  }
  if (key_style_changed) {
    tab->page->ReapplyStyles();
  }
  if (key_style_changed || key_dom_changed) {
    SchedulePendingImageFetch(*tab);
  }
  return not_canceled;
}

void BrowserController::SubmitForm(int tab_id, dom::Element* form)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr || form == nullptr || tab->page == nullptr) {
    return;
  }
  // Everything that reads the live document happens under the DOM lock; the
  // navigation deliberately happens *after* the lock is released.
  std::string target;
  std::optional<PendingCredential> captured;
  {
    const std::unique_lock<std::recursive_mutex> dom_lock = tab->page->AcquireDomLock();
    // Cancelable submit event; the page can preventDefault() to block it.
    if (tab->script_runtime != nullptr &&
        !tab->script_runtime->DispatchCancelableEvent(*form, "submit")) {
      return;
    }
    // Encode the named controls (application/x-www-form-urlencoded) and
    // navigate to the action with the data as the query string (GET).
    const std::string data = CollectFormData(*form);
    // Password manager: capture the login this submission would offer to
    // save (the origin gate below skips file:/data: documents).
    captured = CredentialFromForm(*form);
    const std::string action = std::string(form->GetAttribute("action").value_or(""));
    target = ResolveUrlAgainstBase(action, tab->url);
    if (target.empty()) {
      target = tab->url;
    }
    target += (target.find('?') != std::string::npos ? "&" : "?") + data;
  }
  // Publish the captured login (if any) before navigating: the save prompt
  // belongs to this submission, not to whatever page comes next.
  if (captured.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tab->origin.empty() && tab->origin != "null") {
      captured->origin = tab->origin;
      tab->pending_credential = std::move(captured);
    }
  }
  // Navigate *outside* the DOM lock.  Navigate() replaces the tab's Page, which
  // destroys the Page this lock was taken on; unlocking it afterwards was a
  // use-after-free on the destroyed Page's mutex (reported by ThreadSanitizer,
  // and undefined behaviour per the standard).
  static_cast<void>(Navigate(tab_id, target));
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
  tab->hovered_element = nullptr;
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
  const auto fetch = [this](const url::Url& resource_url, std::string_view) {
    return fetch_(resource_url, CookieHeader(resource_url, NowUnix()));
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

void BrowserController::ProduceFrame(Tab& tab)
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
  tab.page->RasterizeFull(raster, tab.scroll_offset_y, pool_.get());
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
  const int iterations =
      ::neko::browser::PumpScriptTimersUntilQuiet(*tab->script_runtime, max_iterations);
  if (iterations > 0 && tab->page != nullptr) {
    // The pump ran scripts: document.title may have changed (this is how the
    // headless dump-dom path sees a title set by a deferred callback).
    RefreshTabTitle(*tab);
    // Timers may have mutated the DOM; re-run the cascade so the next
    // Layout/Rasterize reflects the new state, and pick up image sources the
    // callbacks assigned (lazy loading after the initial pass).  Batches that
    // only re-pointed media sources (img.src) skip the restyle.
    const bool style_changed = tab->script_runtime->TakeStyleDirty();
    const bool dom_changed = tab->script_runtime->TakeDomDirty();
    if (style_changed) {
      tab->page->ReapplyStyles();
    }
    if (style_changed || dom_changed) {
      SchedulePendingImageFetch(*tab);
    }
  }
  // Deliver any image load/error events the pool queued during the pump.
  // These dispatch page scripts, so they stay inside the DOM lock scope.
  FirePendingImageEvents(*tab);
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
    const bool style_changed = tab->script_runtime->TakeStyleDirty();
    const bool dom_changed = tab->script_runtime->TakeDomDirty();
    if (style_changed) {
      tab->page->ReapplyStyles();
    }
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
    if (tab->script_runtime->RunPendingTimers() > 0) {
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
      const bool style_changed = tab->script_runtime->TakeStyleDirty();
      const bool dom_changed = tab->script_runtime->TakeDomDirty();
      if (style_changed) {
        tab->page->ReapplyStyles();
      }
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
      const bool style_changed = tab->script_runtime->TakeStyleDirty();
      const bool dom_changed = tab->script_runtime->TakeDomDirty();
      if (style_changed) {
        tab->page->ReapplyStyles();
      }
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

void BrowserController::SetTabViewport(int tab_id, int width, int height)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return;
  }
  width = std::max(1, width);
  height = std::max(1, height);
  if (tab->viewport_width == width && tab->viewport_height == height) {
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
    tab->frame_dirty = true;
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

void BrowserController::FetchAndLoad(Tab& tab, const url::Url& url)
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
  // Use the final URL (after redirects) so relative hrefs/srcs in the page
  // resolve against the real document URL, not the pre-redirect request URL.
  const std::string& final_url =
      response.value().final_url.empty() ? url.Serialize() : response.value().final_url;
  LoadBytes(tab, response.value().body, content_type, final_url);
}

void BrowserController::LoadBytes(Tab& tab,
                                  std::string_view bytes,
                                  std::string_view content_type,
                                  const std::string& final_url)
{
  const std::string ct = ToLower(content_type);

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
    const auto fetch_subresource = [this](const url::Url& resource_url, std::string_view) {
      return fetch_(resource_url, CookieHeader(resource_url, NowUnix()));
    };
    // Fetch and apply the page's external <link rel=stylesheet> sheets before
    // scripts run, so scripts see the fully styled cascade.
    FetchExternalStylesheets(*new_page, final_url, fetch_subresource, *pool_);
    // The navigation request lives in the tab (stable address): the runtime's
    // callback keeps writing into it when a timer navigates later.
    tab.pending_script_navigation = {};
    tab.script_runtime = RunPageScripts(
        *new_page,
        final_url,
        [this](const url::Url& script_url) {
          return fetch_(script_url, CookieHeader(script_url, NowUnix()));
        },
        [this](std::string_view level, std::string_view text) { LogConsole(level, text); },
        services,
        &tab.pending_script_navigation);

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
    const std::string existing = std::string(input->GetAttribute("value").value_or(""));
    if (!existing.empty()) {
      return; // the page (or the user) already filled this form: leave it alone
    }
    const storage::Credential& credential = saved.front();
    input->SetAttribute("value", credential.password);
    if (dom::Element* username = FindUsernameInput(inputs, input); username != nullptr) {
      const std::string existing_user = std::string(username->GetAttribute("value").value_or(""));
      if (existing_user.empty() && !credential.username.empty()) {
        username->SetAttribute("value", credential.username);
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
    std::string value = std::string(input->GetAttribute("value").value_or(""));
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
    credential.username = std::string(username->GetAttribute("value").value_or(""));
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

std::string BrowserController::CookieHeader(const url::Url& url, int64_t now) const
{
  return cookies_.CookieHeaderFor(url, now);
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
    const auto response = fetch(parsed.value(), {});
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
    const auto response = fetch(parsed.value(), {});
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
            : fetch(parsed.value(), {});
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
    const auto response = fetch(parsed.value(), {});
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

} // namespace neko::browser
