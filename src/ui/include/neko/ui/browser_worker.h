#pragma once

#include "neko/browser/browser_controller.h"
#include "neko/javascript/script_engine.h"

#include <QObject>
#include <QString>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace neko::base {
class ThreadPool;
}

namespace neko::ui {

// Owns the BrowserController and runs it on a dedicated thread so that the
// synchronous network fetches never block the GUI thread.
//
// Threading: the controller is mutated ONLY on the worker thread.  The GUI
// thread never touches controller internals directly (there is no controller
// accessor); it reads consistent copies through the Snapshot* methods, which
// lock briefly inside the controller and are safe to call at ANY time —
// including from repaint/wheel events while the worker is navigating or
// closing tabs.  StateChanged() is emitted after each action for refresh.
class BrowserWorker : public QObject
{
  Q_OBJECT
public:
  explicit BrowserWorker(QString profile_dir, QObject* parent = nullptr);
  ~BrowserWorker() override;

  // -------------------------------------------------------------------------
  // Thread-safe GUI reads (safe at any time, including while the worker is
  // mid-navigation or closing tabs).
  // -------------------------------------------------------------------------
  std::vector<browser::TabSnapshot> SnapshotTabs() const;
  browser::TabSnapshot SnapshotTab(int id) const;
  browser::TabSnapshot SnapshotActiveTab() const;
  int ActiveTabIndex() const;
  std::vector<storage::HistoryEntry> SnapshotHistory() const;
  std::vector<storage::Bookmark> SnapshotBookmarks() const;
  std::vector<browser::Download> SnapshotDownloads() const;
  size_t SnapshotCookieCount() const;
  std::vector<storage::Cookie> SnapshotCookies() const;
  std::vector<browser::NetworkLogEntry> SnapshotNetworkLog() const;
  std::vector<browser::ConsoleEntry> SnapshotConsoleLog() const;
  std::string profile_dir() const
  {
    return controller_.profile_dir();
  }

  // The controller's shared worker pool (parallel subresource fetching and
  // parallel band rasterization).  Thread-safe; safe to call from the GUI
  // thread.
  base::ThreadPool& pool()
  {
    return controller_.pool();
  }

  // -------------------------------------------------------------------------
  // Actions (thread-safe: queues an action on the worker thread and returns
  // immediately).
  // -------------------------------------------------------------------------
  void Navigate(int tab_id, const QString& input);
  void NavigateActive(const QString& input);
  void Back();
  void Forward();
  void Reload();
  // Dispatches a user click (document coordinates) to the page's script
  // runtime on the worker thread; runs the cancelable "click" event and the
  // default action (hyperlink navigation) unless preventDefault was called.
  void DispatchPointerClick(int tab_id, float doc_x, float doc_y);
  // Fires page-side hover events (mouseover/mouseout) when the hovered element
  // changes; the worker hit-tests |doc_x|,|doc_y| against the current layout.
  void DispatchHover(int tab_id, float doc_x, float doc_y);
  void DispatchHoverClear(int tab_id);
  // Dispatches a wheel event (vertical delta px) to the page's script runtime.
  void DispatchWheel(int tab_id, double delta_y);
  // Dispatches a keyboard event (keydown/keyup) to the page's script runtime.
  void DispatchKeyboard(int tab_id, const QString& type, const QString& key,
                        const QString& code);
  void NewTab(const QString& url, bool activate);
  void CloseTab(int id);
  void ActivateTab(int id);
  void BookmarkActive();
  void RemoveBookmark(const QString& url);
  void Download(const QString& url);
  void ClearStorage();
  void ClearNetworkLog();

  // Runs the active tab's pending page-script timers and advances animated
  // image frames (GIF) on the worker thread (see
  // BrowserController::PumpScriptTimers).  The GUI calls this on a periodic
  // timer so setTimeout/setInterval callbacks and animations progress.
  void PumpScriptTimers();

  // Evaluates |script| in the DevTools console context (persistent global
  // scope, runs on the worker thread so the UI never blocks).  Emits
  // JavaScriptResult() with the formatted output.
  void EvaluateJavaScript(const QString& script);

signals:
  // Emitted (on the worker thread, connected queued) after any action that
  // may have changed state; the GUI should refresh everything.
  void StateChanged();
  void DownloadFinished(qint64 id, bool ok);
  // Console evaluation completed: |script| echoed, |output| formatted
  // result, |error| true when it was a JavaScript error.
  void JavaScriptResult(const QString& script, const QString& output, bool error);

private:
  void Run();
  void Post(std::function<void()> fn);

  browser::BrowserController controller_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool quit_ = false;
  // DevTools console engine; only touched on the worker thread.
  std::unique_ptr<javascript::ScriptEngine> js_engine_;
};

} // namespace neko::ui
