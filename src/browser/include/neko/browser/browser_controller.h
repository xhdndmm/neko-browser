#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "neko/base/status.h"
#include "neko/browser/download_manager.h"
#include "neko/image/image.h"
#include "neko/media/audio.h"
#include "neko/media/media_source.h"
#include "neko/pdf/pdf.h"
#include "neko/renderer/page.h"
#include "neko/storage/bookmark_store.h"
#include "neko/storage/cookie_store.h"
#include "neko/storage/history_store.h"

namespace neko::browser {

// What kind of content a tab is currently showing.
enum class ContentType {
  kHtml,    // rendered through the engine pipeline
  kImage,   // decoded image (PNG/JPEG)
  kPdf,     // extracted PDF text
  kAudio,   // decoded WAV (metadata/samples; no playback yet)
  kText,    // plain text
  kOther,   // opaque binary
  kError,   // navigation/fetch error
};

std::string_view ToString(ContentType type);

// One tab: its navigation history and the current page state.
struct Tab {
  int id = 0;
  std::string url;
  std::string title;
  bool loading = false;

  ContentType content_type = ContentType::kHtml;
  renderer::Page page;            // kHtml
  image::Image image;             // kImage
  pdf::PdfDocument pdf;           // kPdf
  media::AudioData audio;         // kAudio
  std::string raw_text;           // kText / kOther
  std::string error;              // kError

  std::vector<std::string> history;  // back/forward stack (URLs)
  int history_index = -1;

  bool CanGoBack() const { return history_index > 0; }
  bool CanGoForward() const { return history_index >= 0 &&
                                     history_index + 1 < static_cast<int>(history.size()); }
};

// A network request record for DevTools.
struct NetworkLogEntry {
  std::string method = "GET";
  std::string url;
  int status = 0;
  int64_t bytes = 0;
  double elapsed_ms = 0;
  std::string error;  // non-empty when the request failed
  int64_t timestamp = 0;
};

// A console/DevTools message.
struct ConsoleEntry {
  std::string level;  // "info" | "warning" | "error"
  std::string message;
  int64_t timestamp = 0;
};

// The browser application layer: owns tabs, navigation, and the profile
// stores, and exposes a DevTools view of what the engine is doing.  The UI
// (Qt, CLI) talks only to this controller, never to engine internals.
//
// Threading: single-threaded by design.  Network fetches are synchronous;
// a GUI layer may run Navigate()/Start() on a worker thread.
class BrowserController {
 public:
  // Injectable fetch function (tests use a fake; production defaults to
  // network::HttpGet).  |cookie_header| is the computed "Cookie: ..."
  // value (may be empty) for the request host.
  using FetchFn = std::function<base::Result<network::HttpResponse>(
      const url::Url&, std::string_view cookie_header)>;

  explicit BrowserController(std::string profile_dir, FetchFn fetch = {});
  ~BrowserController();

  BrowserController(const BrowserController&) = delete;
  BrowserController& operator=(const BrowserController&) = delete;

  // -------------------------------------------------------------------------
  // Tabs
  // -------------------------------------------------------------------------
  int NewTab(const std::string& url = "", bool activate = true);
  void CloseTab(int id);
  void ActivateTab(int id);
  int active_tab() const { return active_tab_; }
  Tab* ActiveTab();
  const std::vector<std::unique_ptr<Tab>>& tabs() const { return tabs_; }
  Tab* FindTab(int id);

  // -------------------------------------------------------------------------
  // Navigation
  // -------------------------------------------------------------------------
  // Navigates the active tab.  |input| may be an absolute URL, a bare
  // hostname (http:// is prefixed) or a local file path.
  base::Result<void> NavigateActive(const std::string& input);
  base::Result<void> Navigate(int tab_id, const std::string& input);
  void Back();
  void Forward();
  void Reload();

  // Returns the content-type of the active tab.
  ContentType active_content_type() const;

  // -------------------------------------------------------------------------
  // Profile stores
  // -------------------------------------------------------------------------
  storage::CookieStore& cookies() { return cookies_; }
  storage::HistoryStore& history() { return history_; }
  storage::BookmarkStore& bookmarks() { return bookmarks_; }
  DownloadManager& downloads() { return downloads_; }

  base::Result<void> Save();
  base::Result<void> Load();

  // Adds a bookmark for the active tab.
  base::Result<std::string> BookmarkActive(const std::string& folder = "");

  // -------------------------------------------------------------------------
  // DevTools data
  // -------------------------------------------------------------------------
  const std::vector<NetworkLogEntry>& network_log() const { return network_log_; }
  void ClearNetworkLog() { network_log_.clear(); }
  const std::vector<ConsoleEntry>& console_log() const { return console_log_; }
  void LogConsole(std::string_view level, std::string_view message);
  std::string DumpDom() const;         // active tab
  std::string DumpNetworkLog() const;  // text form

  // Resolves user input to a navigable URL string (used by tests).
  std::string ResolveInput(const std::string& input) const;

  const std::string& profile_dir() const { return profile_dir_; }

 private:
  void FetchAndLoad(Tab& tab, const url::Url& url);
  void LoadBytes(Tab& tab, std::string_view bytes, std::string_view content_type,
                 const std::string& final_url);
  void LoadLocalPath(Tab& tab, const std::string& path);
  void RecordVisit(const std::string& url, const std::string& title);
  void NavigateToUrl(Tab& tab, const std::string& url_string);

  std::string profile_dir_;
  FetchFn fetch_;

  std::vector<std::unique_ptr<Tab>> tabs_;
  int active_tab_ = -1;
  int next_tab_id_ = 1;

  storage::CookieStore cookies_;
  storage::HistoryStore history_;
  storage::BookmarkStore bookmarks_;
  DownloadManager downloads_;

  std::vector<NetworkLogEntry> network_log_;
  std::vector<ConsoleEntry> console_log_;
};

// Fetches and decodes the images referenced by <img> elements in |page| and
// attaches them via Page::SetElementImage.  |base_url| resolves relative src
// attributes; failing subresources are skipped silently.  Used by both the
// controller (GUI) and the headless CLI.
void FetchPageImages(renderer::Page& page, const std::string& base_url,
                     const BrowserController::FetchFn& fetch);

}  // namespace neko::browser
