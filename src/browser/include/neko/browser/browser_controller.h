#pragma once

#include "neko/base/status.h"
#include "neko/browser/download_manager.h"
#include "neko/image/image.h"
#include "neko/javascript/dom_binding.h"
#include "neko/media/audio.h"
#include "neko/media/media_source.h"
#include "neko/pdf/pdf.h"
#include "neko/renderer/page.h"
#include "neko/storage/bookmark_store.h"
#include "neko/storage/cookie_store.h"
#include "neko/storage/history_store.h"
#include "neko/storage/local_storage.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace neko::browser {

// What kind of content a tab is currently showing.
enum class ContentType
{
  kHtml,  // rendered through the engine pipeline
  kImage, // decoded image (PNG/JPEG)
  kPdf,   // extracted PDF text
  kAudio, // decoded WAV (metadata/samples; no playback yet)
  kText,  // plain text
  kOther, // opaque binary
  kError, // navigation/fetch error
};

std::string_view ToString(ContentType type);

// One tab: its navigation history and the current page state.
//
// Threading: every member except |id| is written ONLY on the worker thread
// and read by the GUI exclusively through the TabSnapshot copies produced by
// BrowserController::Snapshot*.  Navigation publishes a freshly built
// |page| (and payloads) under the controller mutex; the GUI keeps the shared
// handles alive, so closing or navigating a tab never invalidates what the
// GUI is currently rendering.
struct Tab
{
  int id = 0;
  std::string url;
  std::string title;
  bool loading = false;

  ContentType content_type = ContentType::kHtml;
  // kHtml.  Replaced wholesale by each navigation (never mutated in place
  // after publishing), so a held handle is safe to Layout/Rasterize/read.
  std::shared_ptr<renderer::Page> page;
  std::shared_ptr<image::Image> image;     // kImage
  std::shared_ptr<pdf::PdfDocument> pdf;   // kPdf
  std::shared_ptr<media::AudioData> audio; // kAudio
  std::shared_ptr<std::string> raw_text;   // kText / kOther
  std::shared_ptr<std::string> error;      // kError

  // Back/forward stack; worker-thread only (not exposed to the GUI).
  std::vector<std::string> history;
  int history_index = -1;

  // Live JavaScript runtime for the current HTML page (Phase 8 M2): holds the
  // DOM bindings, timers and event listeners of the page's scripts.  Worker
  // thread only — never exposed to the GUI.  Null for non-HTML content or
  // pages without scripts.
  std::shared_ptr<javascript::DomBinder> script_runtime;

  // The security origin of the current page (scheme+host+port, e.g.
  // "https://example.com"), used by the Same-Origin Policy.  "null" for
  // non-URL content and pages whose URL has no origin.  Worker thread only.
  std::string origin;

  bool CanGoBack() const
  {
    return history_index > 0;
  }
  bool CanGoForward() const
  {
    return history_index >= 0 && history_index + 1 < static_cast<int>(history.size());
  }
};

// A consistent copy of everything the GUI needs to render one tab.  Produced
// under the controller mutex; the snapshot owns its payload through shared
// handles, so it stays valid even if the tab is closed or navigates
// afterwards.  |id| is -1 when no such tab exists.
struct TabSnapshot
{
  int id = -1;
  std::string url;
  std::string title;
  bool loading = false;
  ContentType content_type = ContentType::kHtml;
  // Security origin of the current page ("null" when it has none).
  std::string origin;

  std::shared_ptr<renderer::Page> page;    // kHtml
  std::shared_ptr<image::Image> image;     // kImage
  std::shared_ptr<pdf::PdfDocument> pdf;   // kPdf
  std::shared_ptr<media::AudioData> audio; // kAudio
  std::shared_ptr<std::string> raw_text;   // kText / kOther
  std::shared_ptr<std::string> error;      // kError
};

// A network request record for DevTools.
struct NetworkLogEntry
{
  std::string method = "GET";
  std::string url;
  int status = 0;
  int64_t bytes = 0;
  double elapsed_ms = 0;
  std::string error; // non-empty when the request failed
  int64_t timestamp = 0;
};

// A console/DevTools message.
struct ConsoleEntry
{
  std::string level; // "info" | "warning" | "error"
  std::string message;
  int64_t timestamp = 0;
};

// The browser application layer: owns tabs, navigation, and the profile
// stores, and exposes a DevTools view of what the engine is doing.  The UI
// (Qt, CLI) talks only to this controller, never to engine internals.
//
// Threading: the controller is single-threaded by design — all mutation
// happens on one worker thread (network fetches are synchronous).  The GUI
// thread never touches the controller's internals directly; it reads
// consistent copies through the Snapshot* accessors, which lock |mutex_|
// briefly.  The lock is never held across network fetches or HTML parsing,
// so GUI reads never block on slow navigation.
class BrowserController
{
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
  // Tabs (mutating methods run on the worker thread)
  // -------------------------------------------------------------------------
  int NewTab(const std::string& url = "", bool activate = true);
  void CloseTab(int id);
  void ActivateTab(int id);

  // Worker/test thread only: raw handles into the live tab list.  The GUI
  // must use SnapshotTabs()/SnapshotActiveTab() instead.
  int active_tab() const;
  Tab* ActiveTab();
  const std::vector<std::unique_ptr<Tab>>& tabs() const
  {
    return tabs_;
  }
  Tab* FindTab(int id);

  // -------------------------------------------------------------------------
  // GUI snapshots (thread-safe; lock internally)
  // -------------------------------------------------------------------------
  std::vector<TabSnapshot> SnapshotTabs() const;
  TabSnapshot SnapshotTab(int id) const;
  TabSnapshot SnapshotActiveTab() const;
  std::vector<storage::HistoryEntry> SnapshotHistory() const;
  std::vector<storage::Bookmark> SnapshotBookmarks() const;
  std::vector<Download> SnapshotDownloads() const;
  size_t SnapshotCookieCount() const;
  std::vector<NetworkLogEntry> SnapshotNetworkLog() const;
  std::vector<ConsoleEntry> SnapshotConsoleLog() const;

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

  // Runs the active tab's pending script timers (setTimeout/setInterval) and
  // re-applies the page styles so DOM mutations made by timers are reflected.
  // Worker thread only (thread-confined like the JS runtime).
  void PumpScriptTimers();

  // Returns the content-type of the active tab.
  ContentType active_content_type() const;

  // -------------------------------------------------------------------------
  // Profile stores (worker thread only; GUI reads via Snapshot*)
  // -------------------------------------------------------------------------
  storage::CookieStore& cookies()
  {
    return cookies_;
  }
  storage::HistoryStore& history()
  {
    return history_;
  }
  storage::BookmarkStore& bookmarks()
  {
    return bookmarks_;
  }
  storage::LocalStorage& local_storage()
  {
    return local_storage_;
  }
  DownloadManager& downloads()
  {
    return downloads_;
  }

  // Worker-thread store operations (lock internally).
  base::Result<Download> StartDownload(const url::Url& url, std::string_view cookie_header);
  void RemoveBookmark(const std::string& url);
  void ClearAllStorage();
  std::string CookieHeader(const url::Url& url, int64_t now) const;

  base::Result<void> Save();
  base::Result<void> Load();

  // Adds a bookmark for the active tab.
  base::Result<std::string> BookmarkActive(const std::string& folder = "");

  // -------------------------------------------------------------------------
  // DevTools data (GUI reads via Snapshot*)
  // -------------------------------------------------------------------------
  const std::vector<NetworkLogEntry>& network_log() const
  {
    return network_log_;
  }
  void ClearNetworkLog();
  const std::vector<ConsoleEntry>& console_log() const
  {
    return console_log_;
  }
  void LogConsole(std::string_view level, std::string_view message);
  std::string DumpDom() const;        // active tab
  std::string DumpNetworkLog() const; // text form

  // Resolves user input to a navigable URL string (used by tests).
  std::string ResolveInput(const std::string& input) const;

  const std::string& profile_dir() const
  {
    return profile_dir_;
  }

private:
  void FetchAndLoad(Tab& tab, const url::Url& url);
  void LoadBytes(Tab& tab,
                 std::string_view bytes,
                 std::string_view content_type,
                 const std::string& final_url);
  void LoadLocalPath(Tab& tab, const std::string& path);
  void RecordVisit(const std::string& url, const std::string& title);
  void NavigateToUrl(Tab& tab, const std::string& url_string);

  std::string profile_dir_;
  FetchFn fetch_;

  // Guards every member the GUI can observe through the Snapshot* accessors:
  // tabs_/active_tab_/next_tab_id_, network_log_/console_log_ and the store
  // contents.  Held only around short reads/writes; never across network
  // fetches or HTML parsing.
  mutable std::mutex mutex_;

  std::vector<std::unique_ptr<Tab>> tabs_;
  int active_tab_ = -1;
  int next_tab_id_ = 1;

  storage::CookieStore cookies_;
  storage::HistoryStore history_;
  storage::BookmarkStore bookmarks_;
  storage::LocalStorage local_storage_;
  DownloadManager downloads_;

  std::vector<NetworkLogEntry> network_log_;
  std::vector<ConsoleEntry> console_log_;
};

// Fetches and decodes the images referenced by <img> elements in |page| and
// attaches them via Page::SetElementImage.  |base_url| resolves relative src
// attributes; failing subresources are skipped silently.  Used by both the
// controller (GUI) and the headless CLI.
void FetchPageImages(renderer::Page& page,
                     const std::string& base_url,
                     const BrowserController::FetchFn& fetch);

// Fetches and applies the page's external stylesheets (<link rel="stylesheet">
// href=...): each sheet is fetched and parsed, then registered on |page|
// (re-running the cascade).  |base_url| resolves relative href attributes.
// Used by both the controller (GUI) and the headless CLI.
void FetchExternalStylesheets(renderer::Page& page,
                              const std::string& base_url,
                              const BrowserController::FetchFn& fetch);

} // namespace neko::browser
