#pragma once

#include <QObject>
#include <QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "neko/browser/browser_controller.h"

namespace neko::ui {

// Owns the BrowserController and runs it on a dedicated thread so that the
// synchronous network fetches never block the GUI thread.
//
// Threading: the controller is mutated ONLY on the worker thread; the GUI
// thread reads controller state only after receiving StateChanged() (which
// is delivered through Qt's queued connections, giving a happens-before).
class BrowserWorker : public QObject {
  Q_OBJECT
 public:
  explicit BrowserWorker(QString profile_dir, QObject* parent = nullptr);
  ~BrowserWorker() override;

  // Read-only access for the GUI (only after StateChanged()).
  browser::BrowserController& controller() { return controller_; }

  // Thread-safe: queues an action and returns immediately.
  void Navigate(int tab_id, const QString& input);
  void NavigateActive(const QString& input);
  void Back();
  void Forward();
  void Reload();
  void NewTab(const QString& url, bool activate);
  void CloseTab(int id);
  void ActivateTab(int id);
  void BookmarkActive();
  void RemoveBookmark(const QString& url);
  void Download(const QString& url);
  void ClearStorage();

 signals:
  // Emitted (on the worker thread, connected queued) after any action that
  // may have changed state; the GUI should refresh everything.
  void StateChanged();
  void DownloadFinished(qint64 id, bool ok);

 private:
  void Run();
  void Post(std::function<void()> fn);

  browser::BrowserController controller_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool quit_ = false;
};

}  // namespace neko::ui
