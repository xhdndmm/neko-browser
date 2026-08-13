#include "neko/ui/browser_worker.h"

#include <utility>

#include <QUrl>

#include "neko/base/logging.h"

namespace neko::ui {

BrowserWorker::BrowserWorker(QString profile_dir, QObject* parent)
    : QObject(parent),
      controller_(profile_dir.toStdString()) {
  // Load persisted profile data on the caller (GUI) thread at startup.
  auto loaded = controller_.Load();
  if (!loaded) {
    NEKO_LOG_WARNING("profile load failed: " + loaded.error().message());
  }
  thread_ = std::thread([this] { Run(); });
}

BrowserWorker::~BrowserWorker() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  // controller_ destructor persists the profile.
}

void BrowserWorker::Post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(fn));
  }
  cv_.notify_one();
}

void BrowserWorker::Run() {
  for (;;) {
    std::function<void()> fn;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
      if (quit_ && queue_.empty()) break;
      fn = std::move(queue_.front());
      queue_.pop_front();
    }
    fn();
    emit StateChanged();
  }
}

void BrowserWorker::Navigate(int tab_id, const QString& input) {
  Post([this, tab_id, input = input.toStdString()] {
    (void)controller_.Navigate(tab_id, input);
  });
}

void BrowserWorker::NavigateActive(const QString& input) {
  Post([this, input = input.toStdString()] { (void)controller_.NavigateActive(input); });
}

void BrowserWorker::Back() {
  Post([this] { controller_.Back(); });
}

void BrowserWorker::Forward() {
  Post([this] { controller_.Forward(); });
}

void BrowserWorker::Reload() {
  Post([this] { controller_.Reload(); });
}

void BrowserWorker::NewTab(const QString& url, bool activate) {
  Post([this, url = url.toStdString(), activate] {
    const int id = controller_.NewTab(url, activate);
    if (id >= 0) (void)id;
  });
}

void BrowserWorker::CloseTab(int id) {
  Post([this, id] { controller_.CloseTab(id); });
}

void BrowserWorker::ActivateTab(int id) {
  Post([this, id] { controller_.ActivateTab(id); });
}

void BrowserWorker::BookmarkActive() {
  Post([this] { (void)controller_.BookmarkActive(); });
}

void BrowserWorker::RemoveBookmark(const QString& url) {
  Post([this, url = url.toStdString()] {
    const auto& all = controller_.bookmarks().All();
    for (const auto& b : all) {
      if (b.url == url) {
        controller_.bookmarks().Remove(b.id);
        (void)controller_.Save();
        break;
      }
    }
  });
}

void BrowserWorker::Download(const QString& url) {
  Post([this, url = url.toStdString()] {
    const auto parsed = neko::url::Url::Parse(url);
    if (!parsed.has_value()) {
      emit DownloadFinished(-1, false);
      return;
    }
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto result = controller_.downloads().Start(
        parsed.value(), controller_.cookies().CookieHeaderFor(parsed.value(), now));
    emit DownloadFinished(result.has_value()
                              ? static_cast<qint64>(result.value().id)
                              : -1,
                          result.has_value());
  });
}

void BrowserWorker::ClearStorage() {
  Post([this] {
    controller_.cookies().Clear();
    controller_.history().Clear();
    controller_.bookmarks().Clear();
    controller_.ClearNetworkLog();
    (void)controller_.Save();
  });
}

}  // namespace neko::ui
