#pragma once

#include "neko/base/status.h"
#include "neko/browser/download_manager.h"
#include "neko/browser/page_scripts.h"
#include "neko/browser/renderer_session.h"
#include "neko/image/image.h"
#include "neko/javascript/dom_binding.h"
#include "neko/media/audio.h"
#include "neko/media/media_source.h"
#include "neko/pdf/pdf.h"
#include "neko/renderer/page.h"
#include "neko/storage/bookmark_store.h"
#include "neko/storage/cookie_store.h"
#include "neko/storage/history_store.h"
#include "neko/storage/indexed_db.h"
#include "neko/storage/local_storage.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace neko::base {
class ThreadPool;
}

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

  // The element with keyboard focus (worker-thread only).  Points into the
  // current document; cleared on navigation (the document is replaced).
  dom::Element* focused_element = nullptr;

  // Script-visible session history (window.history): the URLs the page pushed,
  // the index of the current entry, and the last pushed state as JSON.  Worker
  // thread only; reset to the loaded URL on each navigation.  Distinct from the
  // browser back/forward |history| stack, which the UI drives.
  std::vector<std::string> script_history;
  std::size_t script_history_index = 0;
  std::string script_history_state;

  // Scroll bridging state (worker thread).  |scroll_offset_y| is the page's
  // current vertical scroll offset (the GUI reports it via SetTabScrollOffset).  // A
  // script-requested scroll sets |pending_scroll_y| and bumps |scroll_request_id|; the GUI's
  // Refresh() latches the id and applies the requested offset to its scroll bar.  The horizontal
  // scrollbar is disabled, so x is always 0.
  float scroll_offset_y = 0;
  uint64_t scroll_request_id = 0;
  float pending_scroll_y = 0;

  // User page zoom for this tab (Ctrl+=/Ctrl+-); 1.0 = 100%.  Worker thread
  // only; the GUI reads it through TabSnapshot::zoom.
  float zoom = 1.0F;

  // Find-in-page state (Ctrl+F).  |find_query| is the active query ("" = no
  // find session), |find_matches| the in-process match list (renderer-mode tabs
  // leave it empty: their child owns the list), |find_index| the 0-based
  // current match (-1 when none) and |find_match| its rectangle in device
  // pixels.  |find_match_count| mirrors the count for both modes.  Cleared on
  // navigation (the document is replaced).  Guarded by the controller mutex.
  std::string find_query;
  std::vector<renderer::FindMatch> find_matches;
  int find_index = -1;
  renderer::FindMatch find_match;
  int find_match_count = 0;

  // The element the pointer currently hovers over (worker-thread only, used to
  // fire mouseover/mouseout).  Points into the current document; the UI posts
  // pointer positions and the worker hit-tests, so no pointers cross threads.
  dom::Element* hovered_element = nullptr;

  // Live JavaScript runtime for the current HTML page (Phase 8 M2): holds the
  // DOM bindings, timers and event listeners of the page's scripts.  Worker
  // thread only — never exposed to the GUI.  Null for non-HTML content or
  // pages without scripts.
  std::shared_ptr<javascript::DomBinder> script_runtime;

  // A navigation requested by the page's script (window.location assignment,
  // assign()/replace()/reload()).  Owned by the tab — whose address is stable
  // — because the runtime's navigation callback holds a pointer to it: timers
  // may request a navigation long after the load finished, so the storage must
  // outlive RunPageScripts (a stack local used to dangle here).  PumpScriptTimers
  // and LoadBytes act on the request and clear it.  Worker thread only.
  ScriptRequestedNavigation pending_script_navigation;

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

  // ---- Renderer-process mode (ADR 0016 M2) --------------------------------
  // The live renderer child serving this tab's HTML document.  Bound to the
  // site in |session_origin|; replaced when the tab navigates to another site
  // or after a crash.  Worker thread only.
  std::shared_ptr<RendererSession> session;
  std::string session_origin;
  // The most recent frame the child produced for this tab (viewport-sized
  // RGBA8888), shared with the GUI through TabSnapshot.
  std::shared_ptr<const RemoteFrame> remote_frame;
  // Content height reported by the child (drives the GUI's scroll bar) and
  // the geometry the frame was rasterized for.
  float remote_content_height = 0;
  int remote_viewport_width = 0;
  int remote_viewport_height = 0;
  float remote_frame_scroll_y = 0;
  // Hyperlink under the pointer as reported by the child (drives the GUI's
  // pointing-hand cursor; the browser process has no DOM in this mode).
  std::string remote_hover_link;
  // The scroll bar moved since the last frame: pull a fresh one (throttled).
  bool remote_scroll_dirty = false;
  int64_t remote_last_frame_ms = 0;
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

  // Scroll bridging latch: the worker bumps scroll_request_id when a page
  // script requests a scroll; the GUI applies the corresponding
  // pending_scroll_y to its scroll bar when it observes a changed id.
  uint64_t scroll_request_id = 0;
  float pending_scroll_y = 0;

  // ---- Renderer-process mode (ADR 0016 M2) -------------------------------
  // True when this tab's HTML is rendered by a child process.  The GUI then
  // paints |remote_frame| instead of rasterizing |page| (which is null) and
  // takes the scroll range from |remote_content_height|.
  bool remote = false;
  std::shared_ptr<const RemoteFrame> remote_frame;
  float remote_content_height = 0;
  // Hyperlink under the pointer ("" = none), reported by the child.
  std::string remote_hover_link;
  // User zoom factor driving the CSS-pixel mapping (1.0 = 100%).
  float zoom = 1.0F;

  // Find-in-page state for the tab (Ctrl+F).  |find_match_count| is the number
  // of matches, |find_current_index| the 0-based current one (-1 when none) and
  // |find_current_match| its rectangle in device pixels (document coordinates,
  // so the GUI subtracts its scroll offset when drawing the highlight).
  std::string find_query;
  int find_match_count = 0;
  int find_current_index = -1;
  renderer::FindMatch find_current_match;
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

// Renderer-process mode options (ADR 0016 M2).  When enabled, HTML documents
// are rendered by a renderer child process ("isolated rendering mode"): the
// browser fetches the document with its cookies, ships the bytes to the
// child, forwards user input, and displays the frames the child returns.
// Crash isolation: a dying child surfaces as an error page and the next
// navigation spawns a fresh session.
struct RendererOptions
{
  bool enabled = false;
  // Executable that serves --renderer-session.  Empty resolves the CLI binary
  // next to the running executable.
  std::string executable;
};

// The browser zoom ladder (Chrome-like steps between renderer::kMinUserZoom and
// renderer::kMaxUserZoom).  Returns the next factor above |current| when
// |direction| > 0, the next one below for |direction| < 0, or |current| when
// already at the end.  A |current| that sits between two steps snaps to the
// neighbour in the requested direction.
float NextZoomFactor(float current, int direction);

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

  // Renderer-process mode (ADR 0016 M2): see RendererOptions.

  explicit BrowserController(std::string profile_dir,
                             FetchFn fetch = {},
                             RendererOptions renderer = RendererOptions());
  ~BrowserController();

  BrowserController(const BrowserController&) = delete;
  BrowserController& operator=(const BrowserController&) = delete;

  // -------------------------------------------------------------------------
  // Tabs (mutating methods run on the worker thread)
  // -------------------------------------------------------------------------
  int NewTab(const std::string& url = "", bool activate = true);
  void CloseTab(int id);
  void ActivateTab(int id);

  // Dispatches a user click at document coordinates (doc_x, doc_y) on
  // |tab_id|: runs the page's cancelable "click" event on the hit element
  // (full capture->bubble), then — when the event was not canceled — performs
  // the default action (navigate an <a href> hyperlink).  Runs on the worker
  // thread (the page's script runtime is thread-confined).  Returns true when
  // the click was consumed (navigation started or preventDefault).
  bool DispatchPointerClick(int tab_id, float doc_x, float doc_y);

  // Fires mouseout on the previously hovered element and mouseover on the
  // element at |doc_x|,|doc_y| when the hovered element changed (worker thread,
  // hit-testing against the current layout).  The UI posts positions; used for
  // page-side hover reporting (tooltips, hover menus).
  void DispatchHover(int tab_id, float doc_x, float doc_y);

  // Fires mouseout on the hovered element and clears it (pointer left the
  // view).
  void DispatchHoverClear(int tab_id);

  // Dispatches a cancelable wheel event (vertical scroll delta in px) to the
  // tab's focused element (falling back to <body>) on the worker thread.
  // Returns true when NOT canceled.
  bool DispatchWheel(int tab_id, double delta_y);

  // Dispatches a cancelable keyboard event (keydown/keyup) to the tab's
  // focused element (falling back to <body>/document) on the worker thread.
  // Returns true when NOT canceled.
  bool
  DispatchKeyboard(int tab_id, std::string_view type, std::string_view key, std::string_view code);

  // Submits |form| on |tab_id| (worker thread): runs the cancelable "submit"
  // event, then — unless preventDefault was called — collects the named form
  // controls, URL-encodes them (application/x-www-form-urlencoded) and
  // navigates to the form action with the encoded data (GET query).
  void SubmitForm(int tab_id, dom::Element* form);

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
  std::vector<storage::Cookie> SnapshotCookies() const;
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

  // Loads already-fetched document bytes into |tab_id| through the normal
  // content routing (HTML → engine pipeline with page scripts; images / PDF /
  // audio / text → their viewers) using |final_url| as the document URL.
  // Used by the renderer session host — where the browser process fetches and
  // the renderer child parses/renders — and by tests that load bytes directly.
  // Returns the document error when the load failed (parse error, ...).
  base::Result<void> LoadDocument(int tab_id,
                                  std::string_view bytes,
                                  std::string_view content_type,
                                  const std::string& final_url);

  // Runs the active tab's pending script timers (setTimeout/setInterval) and
  // re-applies the page styles so DOM mutations made by timers are reflected.
  // Worker thread only (thread-confined like the JS runtime).
  void PumpScriptTimers();

  // Worker-thread scroll bridging.  |SetTabScrollOffset| records the page's
  // current vertical scroll offset (the GUI reports it from its scroll bar);
  // |SetTabScrollRequest| records a script-requested scroll (window.scrollTo /
  // element.scrollTop assignment) and bumps the request latch the GUI consumes
  // on its next refresh.
  void SetTabScrollOffset(int tab_id, float y);
  void SetTabScrollRequest(int tab_id, float y);

  // Renderer mode: records the viewport the child should lay the page out for
  // (the GUI reports its viewport size) and pulls a fresh frame when it
  // changed.
  void SetTabViewport(int tab_id, int width, int height);

  // ---- Page zoom (Ctrl+= / Ctrl+- / Ctrl+0) -------------------------------
  // Per-tab user zoom, applied to the renderer's CSS-pixel mapping in both
  // execution modes (in-process page or renderer child).  The factor is clamped
  // to [renderer::kMinUserZoom, renderer::kMaxUserZoom]; a navigation keeps the
  // tab's zoom (browser behavior).  Returns the applied factor, or 1.0 for an
  // unknown tab.  NOT IMPLEMENTED: persistence across restarts and per-origin
  // zoom memory (the profile has no preference store yet).
  float SetTabZoom(int tab_id, float factor);
  // Steps one entry up/down the browser zoom ladder (see NextZoomFactor).
  float ZoomInTab(int tab_id);
  float ZoomOutTab(int tab_id);
  float ResetTabZoom(int tab_id);
  float TabZoom(int tab_id) const;

  // ---- Find in page (Ctrl+F) ---------------------------------------------
  // |FindInTab| runs |query| over the tab's laid-out text when |direction| is 0
  // (an empty query clears the state), or steps the match list for +1/-1
  // (wrapping around).  The page scrolls so the current match is visible, and
  // the match list is re-run after each navigation (a reloaded page cannot be
  // searched against a stale list — the state is cleared on navigation).
  // Returns the number of matches.  The current match is exposed through the
  // tab snapshot (device pixels, document coordinates).  NOT IMPLEMENTED:
  // matching across text-run boundaries (a phrase split by a line break or an
  // inline element) and Unicode case folding beyond ASCII.
  int FindInTab(int tab_id, std::string_view query, int direction = 0);
  void ClearFindInTab(int tab_id);

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
  storage::IndexedDbStore& indexed_db()
  {
    return indexed_db_;
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

  // The controller's shared worker pool: parallel subresource fetching
  // (stylesheets/images) and, when rendering, parallel band rasterization.
  // Thread-safe; outlives the controller.
  base::ThreadPool& pool()
  {
    return *pool_;
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

  // Renderer-process mode internals (worker thread only).
  // Ships |bytes| to the tab's renderer session (spawning one for |origin|
  // when needed), applies the resulting status and stores the first frame.
  void LoadHtmlInRenderer(Tab& tab,
                          std::string_view bytes,
                          std::string_view content_type,
                          const std::string& final_url,
                          const std::string& origin);
  // Applies a status reply: URL/title/scroll latches, redirect re-navigation
  // and (when the child flagged a change) a frame pull.  Returns false when
  // the session failed and the caller must stop using |tab|.
  bool ApplyRendererUpdate(Tab& tab, const RendererUpdate& update);
  // Pulls a viewport frame from the tab's session and publishes it (throttled
  // for scroll-driven pulls).
  void PullRemoteFrame(Tab& tab, bool force);
  // Tears the tab's session down and reports |error| as the tab's content.
  void MarkSessionFailed(Tab& tab, std::string_view message);

  std::string profile_dir_;
  FetchFn fetch_;
  RendererOptions renderer_;
  // Resolved path of the renderer executable (empty when resolution failed).
  std::string renderer_executable_;

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
  storage::IndexedDbStore indexed_db_;
  DownloadManager downloads_;

  std::vector<NetworkLogEntry> network_log_;
  std::vector<ConsoleEntry> console_log_;

  // Declared LAST so it is destroyed FIRST: ~ThreadPool drains pending tasks
  // while every store/registry above is still alive (background subresource
  // tasks capture |this|-bound fetch hooks).
  std::unique_ptr<base::ThreadPool> pool_;
};

// Fetches and decodes the images referenced by <img> elements in |page| and
// attaches them via Page::SetElementImage.  |base_url| resolves relative src
// attributes; failing subresources are skipped silently.  Used by both the
// controller (GUI) and the headless CLI.
void FetchPageImages(renderer::Page& page,
                     const std::string& base_url,
                     const BrowserController::FetchFn& fetch,
                     base::ThreadPool& pool);

// Fetches and applies the page's external stylesheets (<link rel="stylesheet">
// href=...): each sheet is fetched and parsed, then registered on |page|
// (re-running the cascade).  |base_url| resolves relative href attributes.
// Used by both the controller (GUI) and the headless CLI.
void FetchExternalStylesheets(renderer::Page& page,
                              const std::string& base_url,
                              const BrowserController::FetchFn& fetch,
                              base::ThreadPool& pool);

// Fetches and registers the page's @font-face web fonts (declared in <style>
// elements and external stylesheets): each font URL is fetched once, decoded
// by FreeType and registered on |page| under its family name; a final
// ReapplyStyles rebuilds text with the new faces.
void FetchWebFonts(renderer::Page& page,
                   const std::string& base_url,
                   const BrowserController::FetchFn& fetch,
                   base::ThreadPool& pool);

// Fetches and decodes the videos referenced by <video src> elements in |page|
// and attaches them via Page::SetElementVideo (first frame + budgeted frame
// strip; autoplay/loop attributes carry over).  |base_url| resolves relative
// src attributes; failing subresources are skipped silently.  Used by both
// the controller (GUI) and the headless CLI.
void FetchPageVideos(renderer::Page& page,
                     const std::string& base_url,
                     const BrowserController::FetchFn& fetch,
                     base::ThreadPool& pool);

} // namespace neko::browser
