#include "neko/browser/browser_controller.h"

#include "neko/base/logging.h"
#include "neko/base/string_util.h"
#include "neko/base/thread_pool.h"
#include "neko/browser/hyperlink.h"
#include "neko/browser/page_scripts.h"
#include "neko/css/parser.h"
#include "neko/dom/element.h"
#include "neko/dom/query.h"
#include "neko/image/image.h"
#include "neko/network/http.h"
#include "neko/security/origin.h"
#include "neko/storage/file_util.h"
#include "neko/url/url.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <future>
#include <optional>
#include <string>
#include <string_view>
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

// Resolves a possibly-relative reference against a base page URL.  Handles
// file:// / bare local path bases (which the URL parser rejects) by string
// concatenation, mirroring HyperlinkTarget.
std::string ResolveUrlAgainstBase(std::string_view ref, std::string_view base_url)
{
  if (ref.empty()) {
    return std::string(base_url);
  }
  const bool is_file = base_url.rfind("file://", 0) == 0;
  const bool is_bare_path = base_url.find(':') == std::string_view::npos;
  if (is_file || is_bare_path) {
    if (ref[0] == '/') {
      return is_file ? "file://" + std::string(ref) : std::string(ref);
    }
    const std::size_t slash = base_url.find_last_of('/');
    return std::string(
               base_url.substr(0, slash != std::string_view::npos ? slash + 1 : base_url.size())) +
           std::string(ref);
  }
  const auto base = url::Url::Parse(base_url);
  if (base.has_value()) {
    const auto resolved = url::Url::Parse(ref, base.value());
    if (resolved.has_value()) {
      return resolved.value().Serialize(/*include_fragment=*/true);
    }
  }
  return std::string(ref);
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
      indexed_db_(profile_dir_), downloads_(profile_dir_ + "/downloads")
{
  pool_ = std::make_unique<base::ThreadPool>();
  // Default fetch: compute cookies for each redirect hop from the controller's
  // cookie jar.  HttpGet invokes HeaderProvider with the current hop URL.
  if (!fetch_) {
    fetch_ = [this](const url::Url& u, std::string_view) {
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
  // Strip any query/fragment from a self-referential form submission (e.g.
  // form.html?q=hello): the file on disk is form.html; the query stays in
  // tab.url so window.location.search reflects it.
  std::string file = path;
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
  if (tab == nullptr || tab->content_type != ContentType::kHtml || tab->page == nullptr) {
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
    // default action (which may navigate away).
    if (tab->script_runtime->TakeDomDirty()) {
      tab->page->ReapplyStyles();
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
  if (tab == nullptr || tab->content_type != ContentType::kHtml || tab->page == nullptr) {
    return;
  }
  const dom::Element* element = tab->page->ElementAt(doc_x, doc_y);
  dom::Element* prev = tab->hovered_element;
  if (element == prev) {
    return;
  }
  if (prev != nullptr && tab->script_runtime != nullptr) {
    tab->script_runtime->DispatchMouseEvent(
        *prev, "mouseout", static_cast<double>(doc_x), static_cast<double>(doc_y), 0);
  }
  tab->hovered_element = const_cast<dom::Element*>(element);
  if (element != nullptr && tab->script_runtime != nullptr) {
    tab->script_runtime->DispatchMouseEvent(*const_cast<dom::Element*>(element),
                                            "mouseover",
                                            static_cast<double>(doc_x),
                                            static_cast<double>(doc_y),
                                            0);
  }
  // A hover handler may mutate the DOM; reflect it.
  if (tab->script_runtime != nullptr && tab->script_runtime->TakeDomDirty()) {
    tab->page->ReapplyStyles();
  }
}

void BrowserController::DispatchHoverClear(int tab_id)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr) {
    return;
  }
  if (tab->hovered_element != nullptr) {
    if (tab->script_runtime != nullptr) {
      tab->script_runtime->DispatchMouseEvent(*tab->hovered_element, "mouseout", 0, 0, 0);
    }
    tab->hovered_element = nullptr;
  }
}

bool BrowserController::DispatchWheel(int tab_id, double delta_y)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr || tab->content_type != ContentType::kHtml || tab->page == nullptr) {
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
  if (tab->script_runtime->TakeDomDirty()) {
    tab->page->ReapplyStyles();
  }
  return not_canceled;
}

bool BrowserController::DispatchKeyboard(int tab_id,
                                         std::string_view type,
                                         std::string_view key,
                                         std::string_view code)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr || tab->content_type != ContentType::kHtml || tab->page == nullptr) {
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
  bool value_changed = false;
  if (input_is_text) {
    const std::string input_type = std::string(control->GetAttribute("type").value_or("text"));
    const bool text_like = input_type == "text" || input_type == "search" ||
                           input_type == "password" || input_type == "email" ||
                           input_type == "url" || input_type == "hidden";
    if (text_like) {
      std::string value = std::string(control->GetAttribute("value").value_or(""));
      if (key == "Backspace") {
        if (!value.empty()) {
          value.pop_back();
        }
      } else if (key == "Enter") {
        if (dom::Element* form = FindEnclosingForm(control)) {
          SubmitForm(tab_id, form);
        }
        return not_canceled;
      } else if (key.size() == 1 && key[0] >= ' ' && key[0] != '\t') {
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
  }
  // Reflect DOM changes from the keydown/input handlers or the value edit
  // (the layout rebuilds and the UI repaints on the next StateChanged).  The
  // value edit always rebuilds, even on pages with no scripts.
  if (value_changed || (tab->script_runtime != nullptr && tab->script_runtime->TakeDomDirty())) {
    tab->page->ReapplyStyles();
  }
  return not_canceled;
}

void BrowserController::SubmitForm(int tab_id, dom::Element* form)
{
  Tab* tab = FindTab(tab_id);
  if (tab == nullptr || form == nullptr || tab->page == nullptr) {
    return;
  }
  // Cancelable submit event; the page can preventDefault() to block it.
  if (tab->script_runtime != nullptr &&
      !tab->script_runtime->DispatchCancelableEvent(*form, "submit")) {
    return;
  }
  // Encode the named controls (application/x-www-form-urlencoded) and navigate
  // to the action with the data as the query string (GET).
  const std::string data = CollectFormData(*form);
  const std::string action = std::string(form->GetAttribute("action").value_or(""));
  std::string target = ResolveUrlAgainstBase(action, tab->url);
  if (target.empty()) {
    target = tab->url;
  }
  target += (target.find('?') != std::string::npos ? "&" : "?") + data;
  static_cast<void>(Navigate(tab_id, target));
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

void BrowserController::PumpScriptTimers()
{
  Tab* tab = ActiveTab();
  if (tab == nullptr) {
    return;
  }
  if (tab->script_runtime != nullptr && tab->script_runtime->RunPendingTimers() > 0 &&
      tab->page != nullptr) {
    // Timers may have mutated the DOM; re-run the cascade so the next
    // Layout/Rasterize reflects the new state.
    tab->page->ReapplyStyles();
  }
  // Animated images (GIF) advance on the same frame clock; a changed frame
  // bumps the page version so the UI repaints.
  if (tab->page != nullptr) {
    (void)tab->page->AdvanceAnimations();
  }
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
    services.indexed_db = &indexed_db_;
    services.cookies = &cookies_;
    services.origin = origin;
    const auto fetch_subresource = [this](const url::Url& resource_url, std::string_view) {
      return fetch_(resource_url, CookieHeader(resource_url, NowUnix()));
    };
    // Fetch and apply the page's external <link rel=stylesheet> sheets before
    // scripts run, so scripts see the fully styled cascade.
    FetchExternalStylesheets(*new_page, final_url, fetch_subresource, *pool_);
    // Fetch and register @font-face web fonts so layout/paint see them.
    FetchWebFonts(*new_page, final_url, fetch_subresource, *pool_);
    browser::ScriptRequestedNavigation requested;
    tab.script_runtime = RunPageScripts(
        *new_page,
        final_url,
        [this](const url::Url& script_url) {
          return fetch_(script_url, CookieHeader(script_url, NowUnix()));
        },
        [this](std::string_view level, std::string_view text) { LogConsole(level, text); },
        services,
        &requested);

    // A script may have requested a navigation (window.location.href=,
    // assign()/replace(), or reload()) — e.g. Baidu's anti-bot page replaces
    // the URL.  Act on it instead of publishing the script's own document;
    // the requested navigation is already resolved to an absolute URL.
    if (!requested.url.empty()) {
      NavigateToUrl(tab, requested.url);
      return;
    }
    if (requested.is_reload && !tab.url.empty()) {
      NavigateToUrl(tab, tab.url);
      return;
    }

    // Fetch and decode the page's <img>/<video> subresources before
    // publishing.
    FetchPageImages(*new_page, final_url, fetch_subresource, *pool_);
    FetchPageVideos(*new_page, final_url, fetch_subresource, *pool_);
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
  if (!r1)
    return r1.error();
  if (!r2)
    return r2.error();
  if (!r3)
    return r3.error();
  if (!r4)
    return r4.error();
  return r5;
}

base::Result<void> BrowserController::Save()
{
  cookies_.PurgeExpired(NowUnix());
  auto r1 = cookies_.Save();
  auto r2 = history_.Save();
  auto r3 = bookmarks_.Save();
  auto r4 = local_storage_.Save();
  auto r5 = indexed_db_.Save();
  if (!r1)
    return r1.error();
  if (!r2)
    return r2.error();
  if (!r3)
    return r3.error();
  if (!r4)
    return r4.error();
  return r5;
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
  indexed_db_.ClearAll();
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

  // Resolve hrefs (document order preserved).
  std::vector<std::string> urls;
  urls.reserve(links.size());
  for (dom::Element* element : links) {
    const std::optional<std::string_view> href = element->GetAttribute("href");
    base::Result<url::Url> target = url::Url::Parse(*href);
    if (base.has_value()) {
      target = url::Url::Parse(*href, base.value());
    }
    if (!target.has_value() ||
        (target.value().scheme() != "http" && target.value().scheme() != "https")) {
      continue;
    }
    urls.push_back(target.value().Serialize());
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
    return base::Ok(css::ParseStyleSheet(response.value().body));
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
  const std::vector<css::FontFaceRule> faces = page.styles().FontFaces();
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

  std::vector<std::future<base::Result<std::vector<uint8_t>>>> futures;
  futures.reserve(pending.size());
  for (const PendingFont& item : pending) {
    futures.push_back(
        pool.Submit([&fetch_bytes, url = item.absolute_url]() { return fetch_bytes(url); }));
  }
  bool loaded_any = false;
  for (std::size_t i = 0; i < pending.size(); ++i) {
    auto bytes = futures[i].get();
    if (!bytes.has_value()) {
      NEKO_LOG_WARNING("font: fetch failed for " + pending[i].absolute_url + ": " +
                       bytes.error().message());
      continue;
    }
    if (page.LoadWebFont(pending[i].rule.family,
                         pending[i].rule.weight,
                         pending[i].rule.italic,
                         pending[i].absolute_url,
                         std::move(bytes.value()))) {
      loaded_any = true;
      NEKO_LOG_INFO("font: registered '" + pending[i].rule.family + "' <- " +
                    pending[i].absolute_url);
    }
  }
  if (loaded_any) {
    page.ReapplyStyles();
  }
}

// ---------------------------------------------------------------------------
// data: URL support for image subresources (`<img src="data:image/png;base64,...">`).
// Only base64 payloads are decoded; percent-encoded text payloads are rare
// for images and are rejected.
// ---------------------------------------------------------------------------

std::optional<std::string> DecodeBase64(std::string_view input)
{
  // -2 = whitespace (skip), -1 = invalid.
  static const std::array<std::int8_t, 256> kReverse = [] {
    std::array<std::int8_t, 256> table{};
    table.fill(-1);
    table[static_cast<unsigned char>('\t')] = -2;
    table[static_cast<unsigned char>('\n')] = -2;
    table[static_cast<unsigned char>('\r')] = -2;
    table[static_cast<unsigned char>(' ')] = -2;
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) {
      table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    }
    return table;
  }();
  std::string out;
  out.reserve(input.size() / 4 * 3);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char raw : input) {
    if (raw == '=') {
      break; // padding: everything after is ignored
    }
    const auto code = kReverse[static_cast<unsigned char>(raw)];
    if (code == -2) {
      continue; // whitespace
    }
    if (code < 0) {
      return std::nullopt; // invalid character
    }
    buffer = (buffer << 6) | static_cast<std::uint32_t>(code);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

bool IsDataUrl(const std::string& url)
{
  return url.rfind("data:", 0) == 0;
}

// Decodes the payload of |url| ("data:[mediatype][;base64],<data>").
std::optional<std::string> DecodeDataUrlBody(const std::string& url)
{
  const std::size_t comma = url.find(',');
  if (comma == std::string::npos) {
    return std::nullopt;
  }
  const std::string_view header(url.data() + 5, comma - 5);
  if (header.find("base64") == std::string_view::npos) {
    return std::nullopt; // percent-encoded text payloads unsupported
  }
  return DecodeBase64(std::string_view(url).substr(comma + 1));
}

void FetchPageImages(renderer::Page& page,
                     const std::string& base_url,
                     const BrowserController::FetchFn& fetch,
                     base::ThreadPool& pool)
{
  dom::Document* doc = page.document();
  if (doc == nullptr) {
    return;
  }
  const base::Result<url::Url> base = url::Url::Parse(base_url);

  // Depth-first walk collecting <img src> and CSS background-image
  // subresources.  background-image URLs come from the computed style (the
  // cascade has run by the time this is called, after scripts).
  struct PendingImage
  {
    dom::Element* element = nullptr;
    std::string url;
  };
  std::vector<PendingImage> pending;
  std::vector<dom::Node*> stack;
  for (dom::Node* child : doc->ChildNodes()) {
    stack.push_back(child);
  }
  const auto collect_url = [&](dom::Element* element, const std::string& raw_url) {
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
  while (!stack.empty()) {
    dom::Node* node = stack.back();
    stack.pop_back();
    if (node->node_type() != dom::NodeType::kElement) {
      continue;
    }
    dom::Element* element = static_cast<dom::Element*>(node);
    if (element->tag_name() == "img") {
      const std::optional<std::string_view> src = element->GetAttribute("src");
      if (src.has_value()) {
        collect_url(element, std::string(*src));
      }
    }
    const style::ComputedStyle& cs = page.styles().StyleFor(*element);
    if (cs.background_image.has_value() && !cs.background_image->empty()) {
      collect_url(element, cs.background_image.value());
    }
    for (dom::Node* child : node->ChildNodes()) {
      stack.push_back(child);
    }
  }
  if (pending.empty()) {
    return;
  }

  // A decoded image plus, for animated GIFs, the full frame set (the Image
  // carries the first frame).
  struct DecodedImage
  {
    image::Image image;
    std::shared_ptr<image::GifAnimation> animation;
  };
  // Decodes an image from raw bytes (shared by the network and data: paths).
  auto decode_bytes = [](const std::string& body) -> base::Result<DecodedImage> {
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
    auto decoded = image::DecodeImage(body);
    if (!decoded.has_value()) {
      return base::Err(decoded.error());
    }
    out.image = std::move(decoded.value());
    return out;
  };
  auto fetch_and_decode = [&fetch,
                           &decode_bytes](const std::string& url) -> base::Result<DecodedImage> {
    if (IsDataUrl(url)) {
      const std::optional<std::string> body = DecodeDataUrlBody(url);
      if (!body.has_value()) {
        return base::Err(base::Error::InvalidArgument("unsupported data URL payload"));
      }
      return decode_bytes(body.value());
    }
    const base::Result<url::Url> parsed = url::Url::Parse(url);
    if (!parsed.has_value()) {
      return base::Err(base::Error::InvalidArgument("invalid image URL"));
    }
    // Images are fetched without a cookie header (the fetch hook receives
    // the same header set as the page's other subresources today).
    const auto response = fetch(parsed.value(), {});
    if (!response) {
      return base::Err(response.error());
    }
    return decode_bytes(response.value().body);
  };

  if (pending.size() == 1) {
    // A single image: do it inline (no thread-pool overhead).
    auto decoded = fetch_and_decode(pending[0].url);
    if (!decoded) {
      NEKO_LOG_WARNING("img: fetch/decode failed for " + pending[0].url);
      return;
    }
    page.SetElementImage(*pending[0].element,
                         std::move(decoded.value().image),
                         std::move(decoded.value().animation));
    NEKO_LOG_INFO("img: injected " + pending[0].url + " -> <" +
                  std::string(pending[0].element->tag_name()) + ">");
    return;
  }

  std::vector<std::future<base::Result<DecodedImage>>> futures;
  futures.reserve(pending.size());
  for (const PendingImage& item : pending) {
    futures.push_back(
        pool.Submit([&fetch_and_decode, url = item.url]() { return fetch_and_decode(url); }));
  }
  for (std::size_t i = 0; i < pending.size(); ++i) {
    auto decoded = futures[i].get();
    if (!decoded) {
      NEKO_LOG_WARNING("img: fetch/decode failed for " + pending[i].url);
      continue;
    }
    page.SetElementImage(*pending[i].element,
                         std::move(decoded.value().image),
                         std::move(decoded.value().animation));
    NEKO_LOG_INFO("img: injected " + pending[i].url + " -> <" +
                  std::string(pending[i].element->tag_name()) + ">");
  }
}

void FetchPageVideos(renderer::Page& page,
                     const std::string& base_url,
                     const BrowserController::FetchFn& fetch,
                     base::ThreadPool& pool)
{
  dom::Document* doc = page.document();
  if (doc == nullptr) {
    return;
  }
  const base::Result<url::Url> base = url::Url::Parse(base_url);

  struct PendingVideo
  {
    dom::Element* element = nullptr;
    std::string url;
    bool autoplay = false;
    bool loop = false;
  };
  std::vector<PendingVideo> pending;
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
    if (element->tag_name() == "video") {
      const std::optional<std::string_view> src = element->GetAttribute("src");
      if (src.has_value() && !src->empty()) {
        base::Result<url::Url> target = url::Url::Parse(std::string(*src));
        if (base.has_value()) {
          target = url::Url::Parse(std::string(*src), base.value());
        }
        if (target.has_value()) {
          PendingVideo item;
          item.element = element;
          item.url = target.value().Serialize();
          item.autoplay = element->HasAttribute("autoplay");
          item.loop = element->HasAttribute("loop");
          pending.push_back(std::move(item));
        } else {
          NEKO_LOG_WARNING("video: cannot resolve url \"" + std::string(*src) + "\"");
        }
      }
    }
    for (dom::Node* child : node->ChildNodes()) {
      stack.push_back(child);
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
      return base::Err(response.error());
    }
    // The decoder is budgeted (frame count + total RGBA bytes), so a huge
    // video degrades to a bounded prefix rather than exhausting memory.
    return media::DecodeVideo(response.value().body);
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
    page.SetElementVideo(*item.element, first_frame, std::move(strip), item.autoplay);
    NEKO_LOG_INFO("video: injected " + item.url + " (" + std::to_string(frame_count) +
                  " frames) -> <" + std::string(item.element->tag_name()) + ">");
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
