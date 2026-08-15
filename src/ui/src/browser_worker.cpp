#include "neko/ui/browser_worker.h"

#include "neko/base/logging.h"

#include <QUrl>
#include <utility>

namespace neko::ui {

BrowserWorker::BrowserWorker(QString profile_dir, QObject* parent)
    : QObject(parent), controller_(profile_dir.toStdString())
{
  // Load persisted profile data on the caller (GUI) thread at startup.
  auto loaded = controller_.Load();
  if (!loaded) {
    NEKO_LOG_WARNING("profile load failed: " + loaded.error().message());
  }
  thread_ = std::thread([this] { Run(); });
}

BrowserWorker::~BrowserWorker()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
  // controller_ destructor persists the profile.
}

void BrowserWorker::Post(std::function<void()> fn)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(fn));
  }
  cv_.notify_one();
}

void BrowserWorker::Run()
{
  for (;;) {
    std::function<void()> fn;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
      if (quit_ && queue_.empty())
        break;
      fn = std::move(queue_.front());
      queue_.pop_front();
    }
    fn();
    emit StateChanged();
  }
}

void BrowserWorker::Navigate(int tab_id, const QString& input)
{
  Post([this, tab_id, input = input.toStdString()] { (void)controller_.Navigate(tab_id, input); });
}

// ---------------------------------------------------------------------------
// GUI snapshots (thread-safe; forward to the controller's locked copies)
// ---------------------------------------------------------------------------

std::vector<browser::TabSnapshot> BrowserWorker::SnapshotTabs() const
{
  return controller_.SnapshotTabs();
}

browser::TabSnapshot BrowserWorker::SnapshotTab(int id) const
{
  return controller_.SnapshotTab(id);
}

browser::TabSnapshot BrowserWorker::SnapshotActiveTab() const
{
  return controller_.SnapshotActiveTab();
}

int BrowserWorker::ActiveTabIndex() const
{
  return controller_.active_tab();
}

std::vector<storage::HistoryEntry> BrowserWorker::SnapshotHistory() const
{
  return controller_.SnapshotHistory();
}

std::vector<storage::Bookmark> BrowserWorker::SnapshotBookmarks() const
{
  return controller_.SnapshotBookmarks();
}

std::vector<browser::Download> BrowserWorker::SnapshotDownloads() const
{
  return controller_.SnapshotDownloads();
}

size_t BrowserWorker::SnapshotCookieCount() const
{
  return controller_.SnapshotCookieCount();
}

std::vector<storage::Cookie> BrowserWorker::SnapshotCookies() const
{
  return controller_.SnapshotCookies();
}

std::vector<browser::NetworkLogEntry> BrowserWorker::SnapshotNetworkLog() const
{
  return controller_.SnapshotNetworkLog();
}

std::vector<browser::ConsoleEntry> BrowserWorker::SnapshotConsoleLog() const
{
  return controller_.SnapshotConsoleLog();
}

void BrowserWorker::NavigateActive(const QString& input)
{
  Post([this, input = input.toStdString()] { (void)controller_.NavigateActive(input); });
}

void BrowserWorker::Back()
{
  Post([this] { controller_.Back(); });
}

void BrowserWorker::Forward()
{
  Post([this] { controller_.Forward(); });
}

void BrowserWorker::Reload()
{
  Post([this] { controller_.Reload(); });
}

void BrowserWorker::PumpScriptTimers()
{
  Post([this] { controller_.PumpScriptTimers(); });
}

void BrowserWorker::NewTab(const QString& url, bool activate)
{
  Post([this, url = url.toStdString(), activate] {
    const int id = controller_.NewTab(url, activate);
    if (id >= 0)
      (void)id;
  });
}

void BrowserWorker::CloseTab(int id)
{
  Post([this, id] { controller_.CloseTab(id); });
}

void BrowserWorker::ActivateTab(int id)
{
  Post([this, id] { controller_.ActivateTab(id); });
}

void BrowserWorker::BookmarkActive()
{
  Post([this] { (void)controller_.BookmarkActive(); });
}

void BrowserWorker::RemoveBookmark(const QString& url)
{
  Post([this, url = url.toStdString()] { controller_.RemoveBookmark(url); });
}

void BrowserWorker::Download(const QString& url)
{
  Post([this, url = url.toStdString()] {
    const auto parsed = neko::url::Url::Parse(url);
    if (!parsed.has_value()) {
      emit DownloadFinished(-1, false);
      return;
    }
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto result =
        controller_.StartDownload(parsed.value(), controller_.CookieHeader(parsed.value(), now));
    emit DownloadFinished(result.has_value() ? static_cast<qint64>(result.value().id) : -1,
                          result.has_value());
  });
}

void BrowserWorker::ClearStorage()
{
  Post([this] { controller_.ClearAllStorage(); });
}

void BrowserWorker::ClearNetworkLog()
{
  Post([this] { controller_.ClearNetworkLog(); });
}

namespace {
// Formats a JavaScript completion value for the DevTools console (objects as
// JSON).  Returns an empty string for undefined.
QString FormatJsResult(const javascript::ScriptValue& value)
{
  if (value.Kind() == javascript::ValueKind::kUndefined)
    return {};
  if (value.Kind() == javascript::ValueKind::kObject) {
    const auto json = value.JsonStringify();
    if (json.has_value())
      return QString::fromStdString(json.value());
  }
  const auto str = value.ToString();
  return str.has_value() ? QString::fromStdString(str.value()) : QString();
}
} // namespace

void BrowserWorker::EvaluateJavaScript(const QString& script)
{
  Post([this, script = script.toStdString()] {
    if (js_engine_ == nullptr) {
      js_engine_ = std::make_unique<javascript::ScriptEngine>();
      js_engine_->SetConsoleSink([this](std::string_view /*level*/, std::string_view text) {
        emit JavaScriptResult(
            QString(), QString::fromUtf8(text.data(), static_cast<int>(text.size())), false);
      });
    }
    auto result = js_engine_->Evaluate(script);
    if (!result.has_value()) {
      emit JavaScriptResult(
          QString::fromStdString(script), QString::fromStdString(result.error().message()), true);
      return;
    }
    emit JavaScriptResult(QString::fromStdString(script), FormatJsResult(result.value()), false);
  });
}

} // namespace neko::ui
