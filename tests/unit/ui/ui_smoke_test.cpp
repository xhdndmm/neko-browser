// GUI smoke tests.  They run under the Qt "offscreen" platform plugin
// (QT_QPA_PLATFORM=offscreen, set as a ctest property) so no display is
// needed.  The tests exercise the real BrowserWorker + MainWindow + WebView
// stack end to end.

#include "neko/browser/browser_controller.h"
#include "neko/dom/query.h"
#include "neko/layout/layout_tree.h"
#include "neko/storage/file_util.h"
#include "neko/storage/password_store.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/i18n.h"
#include "neko/ui/main_window.h"
#include "neko/ui/web_view.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>
#include <QTranslator>
#include <QWheelEvent>
#include <QWidget>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

// True in ASan/TSan builds: their allocators replace malloc and do not
// implement the glibc heap trim, so RSS assertions would be meaningless.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define NEKO_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define NEKO_TEST_SANITIZED 1
#endif
#endif

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

int main(int argc, char** argv)
{
  // These tests must run without a display; force the offscreen platform
  // unless the caller already chose one (ctest discovery also runs this
  // binary, so the variable must be set here rather than only as a test
  // property).
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

// Temp directory names must be unique per process.  MSVC exposes the process
// id as _getpid (<process.h>), POSIX as getpid (<unistd.h>).
long CurrentProcessId()
{
#ifdef _WIN32
  return static_cast<long>(::_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

class TempProfile
{
public:
  TempProfile()
  {
    dir_ = std::filesystem::temp_directory_path() /
           ("neko-ui-test-" + std::to_string(CurrentProcessId()) + "-" + std::to_string(++seq_));
    std::filesystem::create_directories(dir_);
  }
  ~TempProfile()
  {
    std::filesystem::remove_all(dir_);
  }
  const std::string path() const
  {
    return dir_.string();
  }

private:
  static int seq_;
  std::filesystem::path dir_;
};
int TempProfile::seq_ = 0;

// Pumps the event loop while polling a GUI-thread predicate (safe because
// the predicate only reads widgets, which are owned by the GUI thread).
template <typename Predicate> bool WaitFor(Predicate predicate, int timeout_ms = 5000)
{
  const int step = 20;
  int elapsed = 0;
  while (elapsed < timeout_ms) {
    QCoreApplication::processEvents();
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(step));
    elapsed += step;
  }
  return predicate();
}

// Holds a Page's DOM lock for as long as it is in scope.
//
// The worker thread replaces the layout tree and the style cache under that lock
// (Page::LayoutLocked), so a test that inspects |Page| state from the test thread
// has to take it too.  The GUI never needs this: per ADR 0020 it consumes
// immutable frames rather than the live tree.  Without the lock, a predicate like
// "is the page laid out yet?" races the worker's relayout -- which ThreadSanitizer
// reports as a data race on Page::root_.
class PageDomReadLock
{
public:
  explicit PageDomReadLock(const std::shared_ptr<neko::renderer::Page>& page)
      : lock_(page != nullptr ? page->AcquireDomLock() : std::unique_lock<std::recursive_mutex>())
  {}
  PageDomReadLock(const PageDomReadLock&) = delete;
  PageDomReadLock& operator=(const PageDomReadLock&) = delete;

private:
  std::unique_lock<std::recursive_mutex> lock_;
};

// Sends a real key press+release pair to |widget| through the Qt event
// system, exactly as the platform would deliver it.
void SendKey(QWidget* widget, int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
  QKeyEvent press(QEvent::KeyPress, key, mods);
  QKeyEvent release(QEvent::KeyRelease, key, mods);
  QApplication::sendEvent(widget, &press);
  QApplication::sendEvent(widget, &release);
}

TEST(UiSmokeTest, RendersLocalHtmlPage)
{
  TempProfile tp;
  const std::string html_file = tp.path() + "/page.html";
  ASSERT_TRUE(
      neko::storage::WriteFileAtomic(html_file,
                                     "<html><head><title>UI Test</title></head>"
                                     "<body><h1>Hello UI</h1><p>body text</p></body></html>")
          .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();

  // Navigate through the worker (same path the address bar uses).
  worker.NavigateActive(QString::fromStdString(html_file));
  // The offscreen platform keeps the address bar focused, which suppresses
  // URL sync; drop focus to mimic the user clicking the page.
  window.AddressBar()->clearFocus();

  // Wait until the GUI has refreshed and shows the page title in the tab
  // bar (which implies the controller finished routing/parsing it).
  ASSERT_TRUE(WaitFor([&] {
    for (int i = 0; i < window.TabBarWidget()->count(); ++i) {
      if (window.TabBarWidget()->tabText(i).contains("UI Test"))
        return true;
    }
    return false;
  }));

  // The controller routed and parsed the page (read through the thread-safe
  // snapshot API, the same path the GUI uses).
  EXPECT_EQ(worker.SnapshotActiveTab().title, "UI Test");
  EXPECT_EQ(worker.SnapshotActiveTab().content_type, neko::browser::ContentType::kHtml);

  // The address bar shows the file path and history has one entry.
  EXPECT_GE(window.TabBarWidget()->count(), 1);
  EXPECT_FALSE(window.AddressBar()->text().isEmpty());
  EXPECT_EQ(worker.SnapshotHistory().size(), 1u);

  // Render the view to an image and verify it is non-blank.
  const QPixmap shot = window.grab();
  EXPECT_FALSE(shot.isNull());
  EXPECT_GT(shot.width(), 0);
}

TEST(UiSmokeTest, NavigationUpdatesAddressBarAndHistory)
{
  TempProfile tp;
  const std::string html_file = tp.path() + "/nav.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  html_file, "<html><head><title>Nav</title></head><body>ok</body></html>")
                  .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();

  worker.NavigateActive(QString::fromStdString(html_file));

  // The address bar is updated on the GUI thread after navigation.  In the
  // offscreen platform the address bar keeps focus, which suppresses the
  // URL sync (the real GUI loses focus when the user clicks the page); mimic
  // that by dropping focus so the refresh can write the URL.
  window.AddressBar()->clearFocus();
  ASSERT_TRUE(WaitFor([&] { return window.AddressBar()->text().contains("nav.html"); }));
  ASSERT_EQ(worker.SnapshotHistory().size(), 1u);
  EXPECT_NE(window.AddressBar()->text().toStdString().find("nav.html"), std::string::npos);
}

TEST(UiSmokeTest, BackUpdatesAddressBar)
{
  TempProfile tp;
  const std::string a = tp.path() + "/a.html";
  const std::string b = tp.path() + "/b.html";
  ASSERT_TRUE(
      neko::storage::WriteFileAtomic(a, "<html><head><title>A</title></head><body>a</body></html>")
          .has_value());
  ASSERT_TRUE(
      neko::storage::WriteFileAtomic(b, "<html><head><title>B</title></head><body>b</body></html>")
          .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();

  // The offscreen platform keeps the address bar focused, which suppresses
  // URL sync; drop focus to mimic the user clicking the page.
  auto lose_focus = [&] { window.AddressBar()->clearFocus(); };

  worker.NavigateActive(QString::fromStdString(a));
  lose_focus();
  ASSERT_TRUE(WaitFor([&] { return window.AddressBar()->text().contains("a.html"); }));
  worker.NavigateActive(QString::fromStdString(b));
  lose_focus();
  ASSERT_TRUE(WaitFor([&] { return window.AddressBar()->text().contains("b.html"); }));

  // Going back must refresh the address bar to the previous page's URL.
  worker.Back();
  lose_focus();
  ASSERT_TRUE(WaitFor([&] { return window.AddressBar()->text().contains("a.html"); }));
}

TEST(UiSmokeTest, DevToolsConsoleEvaluatesJavaScript)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();

  // Type into the DevTools console input and "press Enter" (the real path
  // the user takes: QLineEdit::returnPressed -> OnConsoleCommand -> worker).
  QLineEdit* input = window.ConsoleInput();
  ASSERT_NE(input, nullptr);
  input->setText("21 * 2");
  emit input->returnPressed();

  // The worker evaluates on its thread and emits JavaScriptResult; the GUI
  // echoes the input and appends the result to the console view.
  ASSERT_TRUE(WaitFor([&] {
    const QString text = window.ConsoleView()->toPlainText();
    return text.contains("21 * 2") && text.contains("42");
  }));

  // Errors are echoed as errors.
  input->setText("throw new Error('ui boom')");
  emit input->returnPressed();
  ASSERT_TRUE(
      WaitFor([&] { return window.ConsoleView()->toPlainText().contains("Error: ui boom"); }));
}

TEST(UiSmokeTest, AddressBarBackspaceDeletesCharacter)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();

  QLineEdit* address = window.AddressBar();
  ASSERT_NE(address, nullptr);
  // Put a URL in the bar exactly as RefreshAll would (programmatic write).
  address->setText("https://example.com/foo");
  address->setFocus();
  // Place the cursor at the end of the text, then delete the last character
  // with Backspace — a plain QLineEdit must honor this.
  address->setCursorPosition(static_cast<int>(address->text().size()));
  SendKey(address, Qt::Key_Backspace);
  EXPECT_EQ(address->text(), QStringLiteral("https://example.com/fo"));
}

TEST(UiSmokeTest, AddressBarEditSurvivesPeriodicRefresh)
{
  TempProfile tp;
  const std::string html_file = tp.path() + "/edit.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  html_file, "<html><head><title>Edit</title></head><body>ok</body></html>")
                  .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();

  // Navigate so the address bar is populated with a real URL, then simulate
  // the user clicking into the bar (focus + cursor) and editing it.  The
  // periodic StateChanged refresh (script timer) must not clobber an
  // in-progress edit or reset the cursor position.
  worker.NavigateActive(QString::fromStdString(html_file));
  ASSERT_TRUE(WaitFor([&] { return window.AddressBar()->text().contains("edit.html"); }));

  QLineEdit* address = window.AddressBar();
  address->setFocus();
  // Click in the MIDDLE of the text (a user editing the path, not the end).
  const QString original = address->text();
  const int mid = static_cast<int>(original.size()) / 2;
  address->setCursorPosition(mid);

  // Give the periodic refresh timer a chance to fire several times.  The
  // cursor must not be reset to the end (that is what makes typed edits land
  // in the wrong place and makes Backspace appear to "not delete").
  for (int i = 0; i < 6; ++i) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  EXPECT_EQ(address->text(), original);
  EXPECT_EQ(address->cursorPosition(), mid)
      << "periodic refresh must not reset the address-bar cursor";

  // Backspace must delete the character before the cursor.
  SendKey(address, Qt::Key_Backspace);
  EXPECT_EQ(address->text().size() + 1, original.size());
  EXPECT_EQ(address->text(), original.left(mid - 1) + original.mid(mid));
}

TEST(UiSmokeTest, MultiTabNavigationWorks)
{
  TempProfile tp;
  const std::string a = tp.path() + "/tab_a.html";
  const std::string b = tp.path() + "/tab_b.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  a, "<html><head><title>TabA</title></head><body>a</body></html>")
                  .has_value());
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  b, "<html><head><title>TabB</title></head><body>b</body></html>")
                  .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();

  // The initial tab is created asynchronously on the worker thread; wait for
  // the GUI to pick it up (a real launch race the GUI must survive).
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));
  ASSERT_EQ(window.TabBarWidget()->count(), 1);

  // Open a second tab and navigate each tab to a different page.
  window.TabBarWidget()->setCurrentIndex(0);
  worker.NavigateActive(QString::fromStdString(a));
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->tabText(0).contains("TabA"); }));

  worker.NewTab("", true);
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 2; }));
  ASSERT_EQ(window.TabBarWidget()->count(), 2);
  ASSERT_EQ(window.TabBarWidget()->currentIndex(), 1);
  worker.NavigateActive(QString::fromStdString(b));
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->tabText(1).contains("TabB"); }));

  // Switching back to tab 0 must show tab A's title/URL again.
  window.TabBarWidget()->setCurrentIndex(0);
  QCoreApplication::processEvents();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  QCoreApplication::processEvents();
  EXPECT_EQ(worker.SnapshotTab(worker.SnapshotTabs()[0].id).title, "TabA");
}

TEST(UiSmokeTest, KeyboardShortcutOpensAndClosesTabs)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();

  // The initial tab is created asynchronously.
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));
  const int initial = window.TabBarWidget()->count();

  // Ctrl+T opens a new tab (the same path the "+" button uses).
  SendKey(&window, Qt::Key_T, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= initial + 1; }));
  EXPECT_EQ(window.TabBarWidget()->count(), initial + 1);

  // Ctrl+W closes the active tab.
  SendKey(&window, Qt::Key_W, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() <= initial; }));
  EXPECT_EQ(window.TabBarWidget()->count(), initial);
}

// ---------------------------------------------------------------------------
// Browser chrome: settings, bookmark bar, history management, context menu
// ---------------------------------------------------------------------------

TEST(UiSmokeTest, SearchEngineSettingPersists)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));
  ASSERT_NE(window.SearchEngineComboWidget(), nullptr);

  const int bing = window.SearchEngineComboWidget()->findData(QStringLiteral("bing"));
  ASSERT_GE(bing, 0);
  window.SearchEngineComboWidget()->setCurrentIndex(bing);
  EXPECT_TRUE(WaitFor([&] {
    for (const auto& [key, value] : worker.SnapshotPreferences()) {
      if (key == "search_engine") {
        return value == "bing";
      }
    }
    return false;
  }));
}

TEST(UiSmokeTest, BookmarkBarShowsBookmarksAndNavigates)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));

  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("neko-bmbar-" + std::to_string(CurrentProcessId()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path page_a = dir / "bm-a.html";
  const std::filesystem::path page_b = dir / "bm-b.html";
  ASSERT_TRUE(
      neko::storage::WriteFileAtomic(
          page_a.string(), "<html><head><title>Bookmarked A</title></head><body>a</body></html>")
          .has_value());
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  page_b.string(), "<html><head><title>Other B</title></head><body>b</body></html>")
                  .has_value());

  worker.NavigateActive(QString::fromStdString(page_a.string()));
  ASSERT_TRUE(WaitFor([&] {
    const auto tab = worker.SnapshotActiveTab();
    return tab.page != nullptr && !tab.loading;
  }));
  worker.BookmarkActive();
  ASSERT_TRUE(WaitFor([&] { return !worker.SnapshotBookmarks().empty(); }));

  QToolBar* bar = window.BookmarkBarWidget();
  ASSERT_NE(bar, nullptr);
  QAction* bookmark_action = nullptr;
  EXPECT_TRUE(WaitFor([&] {
    for (QAction* action : bar->actions()) {
      if (action->data().toString().contains("bm-a.html")) {
        bookmark_action = action;
        return true;
      }
    }
    return false;
  }));
  ASSERT_NE(bookmark_action, nullptr);

  // Navigate away, then click the bookmark in the bar to come back.
  worker.NavigateActive(QString::fromStdString(page_b.string()));
  ASSERT_TRUE(WaitFor([&] { return worker.SnapshotActiveTab().title == "Other B"; }));
  bookmark_action->trigger();
  EXPECT_TRUE(WaitFor([&] { return worker.SnapshotActiveTab().title == "Bookmarked A"; }));

  std::filesystem::remove_all(dir);
}

TEST(UiSmokeTest, BookmarkBarVisibilityFollowsPreference)
{
  TempProfile tp;
  {
    neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
    neko::ui::MainWindow window(&worker);
    window.show();
    ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));
    ASSERT_NE(window.BookmarkBarCheckWidget(), nullptr);
    ASSERT_TRUE(window.BookmarkBarWidget()->isVisible());

    window.BookmarkBarCheckWidget()->setChecked(false);
    EXPECT_TRUE(WaitFor([&] { return !window.BookmarkBarWidget()->isVisible(); }));
    EXPECT_TRUE(WaitFor([&] {
      for (const auto& [key, value] : worker.SnapshotPreferences()) {
        if (key == "show_bookmark_bar") {
          return value == "0";
        }
      }
      return false;
    }));
  }
  {
    // A fresh worker on the same profile restores the hidden bar.
    neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
    neko::ui::MainWindow window(&worker);
    window.show();
    ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));
    EXPECT_FALSE(window.BookmarkBarWidget()->isVisible());
  }
}

TEST(UiSmokeTest, HistorySearchAndDelete)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));

  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("neko-hist-" + std::to_string(CurrentProcessId()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path page_a = dir / "hist-a.html";
  const std::filesystem::path page_b = dir / "hist-b.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  page_a.string(), "<html><head><title>Hist A</title></head><body>a</body></html>")
                  .has_value());
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  page_b.string(), "<html><head><title>Hist B</title></head><body>b</body></html>")
                  .has_value());
  worker.NavigateActive(QString::fromStdString(page_a.string()));
  ASSERT_TRUE(WaitFor([&] { return worker.SnapshotActiveTab().title == "Hist A"; }));
  worker.NavigateActive(QString::fromStdString(page_b.string()));
  ASSERT_TRUE(WaitFor([&] { return worker.SnapshotActiveTab().title == "Hist B"; }));
  ASSERT_TRUE(WaitFor([&] { return worker.SnapshotHistory().size() >= 2; }));

  // Filtering narrows the list to the matching page.
  ASSERT_NE(window.HistorySearchWidget(), nullptr);
  window.HistorySearchWidget()->setText(QStringLiteral("hist-a"));
  QCoreApplication::processEvents();
  EXPECT_EQ(window.HistoryListWidget()->count(), 1);
  EXPECT_TRUE(
      window.HistoryListWidget()->item(0)->data(Qt::UserRole).toString().contains("hist-a"));

  // Deleting the selected entry removes it from the store.
  window.HistoryListWidget()->setCurrentRow(0);
  QMetaObject::invokeMethod(&window, "OnHistoryDeleteSelected");
  EXPECT_TRUE(WaitFor([&] {
    for (const auto& entry : worker.SnapshotHistory()) {
      if (entry.url.find("hist-a") != std::string::npos) {
        return false;
      }
    }
    return true;
  }));

  window.HistorySearchWidget()->clear();
  QCoreApplication::processEvents();
  std::filesystem::remove_all(dir);
}

TEST(UiSmokeTest, ContextMenuOffersLinkActions)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));

  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("neko-ctx-" + std::to_string(CurrentProcessId()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path page_a = dir / "ctx-a.html";
  const std::filesystem::path page_b = dir / "ctx-b.html";
  ASSERT_TRUE(
      neko::storage::WriteFileAtomic(page_a.string(),
                                     "<html><head><title>Ctx A</title></head><body>"
                                     "<a id=\"link\" href=\"ctx-b.html\">go</a></body></html>")
          .has_value());
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  page_b.string(), "<html><head><title>Ctx B</title></head><body>b</body></html>")
                  .has_value());

  worker.NavigateActive(QString::fromStdString(page_a.string()));
  ASSERT_TRUE(WaitFor([&] { return worker.SnapshotActiveTab().title == "Ctx A"; }));
  // The context menu hit-tests in the worker's document coordinates, so wait
  // until the worker has laid the page out for the real viewport width (1:1
  // with device pixels at the default zoom); the probe below then measures at
  // the same width the hit test uses.
  neko::ui::WebView* view = window.ActiveView();
  ASSERT_NE(view, nullptr);
  const int viewport_width = view->viewport()->width();
  ASSERT_TRUE(WaitFor([&] {
    const neko::browser::TabSnapshot snapshot = worker.SnapshotActiveTab();
    return snapshot.frame != nullptr && snapshot.frame->width == viewport_width;
  }));
  const neko::browser::TabSnapshot framed = worker.SnapshotActiveTab();

  // Compute the link's viewport position from the same engine the view uses
  // (the frame is 1:1 with document pixels at scroll offset 0).
  neko::renderer::Page probe;
  ASSERT_TRUE(probe.LoadHtml(neko::storage::ReadFile(page_a.string()).value()).has_value());
  probe.Layout(static_cast<float>(framed.frame->width), static_cast<float>(framed.frame->height));
  neko::dom::Element* link = neko::dom::QuerySelector(*probe.document(), "#link");
  ASSERT_NE(link, nullptr);
  const auto geometry = probe.ElementBoxGeometry(*link);
  ASSERT_TRUE(geometry.has_value());
  const QPoint point(static_cast<int>(geometry->x + geometry->width / 2.0F),
                     static_cast<int>(geometry->y + geometry->height / 2.0F));
  // Sanity-check the coordinates against the worker's own hit test before
  // exercising the menu.
  const neko::browser::PointInfo hit =
      worker.QueryPoint(framed.id, static_cast<float>(point.x()), static_cast<float>(point.y()));
  ASSERT_FALSE(hit.link_url.empty()) << "probe coordinates must hit the link";

  QMenu* menu = view->CreatePageContextMenu(point);
  ASSERT_NE(menu, nullptr);
  QStringList entries;
  QAction* open_link = nullptr;
  QAction* copy_link = nullptr;
  for (QAction* action : menu->actions()) {
    entries << action->text();
    if (action->text() == QStringLiteral("Open Link in New Tab")) {
      open_link = action;
    }
    if (action->text() == QStringLiteral("Copy Link Address")) {
      copy_link = action;
    }
  }
  EXPECT_TRUE(entries.contains(QStringLiteral("Open Link in New Tab")));
  EXPECT_TRUE(entries.contains(QStringLiteral("Copy Link Address")));
  EXPECT_TRUE(entries.contains(QStringLiteral("Back")));

  // Copy Link Address puts the resolved target on the clipboard.
  ASSERT_NE(copy_link, nullptr);
  copy_link->trigger();
  EXPECT_TRUE(QGuiApplication::clipboard()->text().contains("ctx-b.html"));

  // Open Link in New Tab opens the second page in a new tab.
  ASSERT_NE(open_link, nullptr);
  open_link->trigger();
  EXPECT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() == 2; }));
  // The menu is owned by the caller (parented to the window so that the
  // WebView rebuild triggered by opening the tab cannot destroy it mid-flight);
  // delete it synchronously, without another event-loop turn.
  delete menu;

  std::filesystem::remove_all(dir);
}

// Regression: closing a tab must release its Page (DOM, style store, display
// list, frame buffers).  A stale reference here means every closed tab keeps
// its whole page alive for the browser's lifetime.
TEST(UiSmokeTest, ClosingTabReleasesThePage)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));

  // Enough content that keeping it alive is meaningful (and visible).
  std::string html = "<html><head><title>Leak probe</title></head><body>";
  for (int i = 0; i < 2000; ++i) {
    html += "<div class=\"row\"><span>Item " + std::to_string(i) + "</span></div>";
  }
  html += "</body></html>";
  const std::filesystem::path page =
      std::filesystem::temp_directory_path() /
      ("neko-close-tab-" + std::to_string(CurrentProcessId()) + ".html");
  ASSERT_TRUE(neko::storage::WriteFileAtomic(page.string(), html).has_value());

  worker.NewTab(QString::fromStdString(page.string()), true);
  std::weak_ptr<neko::renderer::Page> weak;
  int id = -1;
  ASSERT_TRUE(WaitFor([&] {
    const neko::browser::TabSnapshot snapshot = worker.SnapshotActiveTab();
    if (snapshot.id < 0 || snapshot.page == nullptr || snapshot.loading) {
      return false;
    }
    weak = snapshot.page;
    id = snapshot.id;
    return true;
  }));
  ASSERT_EQ(weak.expired(), false);

  worker.CloseTab(id);
  // The page must be destroyed while the browser keeps running -- not merely
  // at worker destruction.
  EXPECT_TRUE(WaitFor([&] { return weak.expired(); }, 5000))
      << "the closed tab's Page is still referenced somewhere";

  std::filesystem::remove(page);
}

// Same contract through the full GUI stack (MainWindow + WebView): closing
// the window's tab must drop every reference the widgets hold to the page.
TEST(UiSmokeTest, ClosingTabInWindowReleasesThePage)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));

  std::string html = "<html><head><title>Window leak probe</title></head><body>";
  for (int i = 0; i < 2000; ++i) {
    html += "<div class=\"row\"><span>Item " + std::to_string(i) + "</span></div>";
  }
  html += "</body></html>";
  const std::filesystem::path page =
      std::filesystem::temp_directory_path() /
      ("neko-close-tab-window-" + std::to_string(CurrentProcessId()) + ".html");
  ASSERT_TRUE(neko::storage::WriteFileAtomic(page.string(), html).has_value());

  window.TabBarWidget()->setCurrentIndex(0);
  worker.NavigateActive(QString::fromStdString(page.string()));
  std::weak_ptr<neko::renderer::Page> weak;
  ASSERT_TRUE(WaitFor([&] {
    const neko::browser::TabSnapshot snapshot = worker.SnapshotActiveTab();
    if (snapshot.page == nullptr || snapshot.loading) {
      return false;
    }
    weak = snapshot.page;
    return true;
  }));
  ASSERT_EQ(weak.expired(), false);

  SendKey(&window, Qt::Key_W, Qt::ControlModifier);
  EXPECT_TRUE(WaitFor([&] { return weak.expired(); }, 5000))
      << "the closed tab's Page is still referenced by the GUI stack";

  std::filesystem::remove(page);
}

#if defined(__linux__)
// Current process RSS in KiB; used by the memory-return observation below.
long CurrentRssKb()
{
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      return std::stol(line.substr(6));
    }
  }
  return -1;
}
#endif

// Regression + observation (Linux/glibc): loading a heavy page in the GUI and
// closing its tab must return the freed memory to the OS.  Object lifetime is
// covered by the tests above; this checks the allocator side.  The controller
// calls base::ReleaseFreeMemory() when a tab closes because glibc otherwise
// keeps the freed DOM/style/image blocks in its arenas and the RSS barely
// moves (measured before the fix: 1 MiB released of 126 MiB loaded).
// Sanitizer allocators do not implement the trim, so those builds only print
// the numbers; the ratio is asserted only when the load actually grew the
// resident set by a meaningful amount.
TEST(UiSmokeTest, ClosingTabsReturnsMemory)
{
#if !defined(__linux__) || !defined(__GLIBC__)
  GTEST_SKIP() << "RSS assertion requires Linux + glibc";
#else
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.show();
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() >= 1; }));

  std::string html = "<html><head><title>Memory probe</title></head><body>";
  for (int i = 0; i < 10000; ++i) {
    html += "<div class=\"row\"><span>Item " + std::to_string(i) + "</span></div>";
  }
  html += "</body></html>";
  const std::filesystem::path page =
      std::filesystem::temp_directory_path() /
      ("neko-mem-probe-" + std::to_string(CurrentProcessId()) + ".html");
  ASSERT_TRUE(neko::storage::WriteFileAtomic(page.string(), html).has_value());

  const long rss_before = CurrentRssKb();
  window.TabBarWidget()->setCurrentIndex(0);
  worker.NavigateActive(QString::fromStdString(page.string()));
  ASSERT_TRUE(WaitFor(
      [&] {
        const neko::browser::TabSnapshot snapshot = worker.SnapshotActiveTab();
        return snapshot.page != nullptr && !snapshot.loading;
      },
      15000));
  for (int i = 0; i < 10; ++i) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const long rss_loaded = CurrentRssKb();

  SendKey(&window, Qt::Key_W, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.TabBarWidget()->count() == 1; }));
  for (int i = 0; i < 25; ++i) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const long rss_closed = CurrentRssKb();

  const long gained = rss_loaded - rss_before;
  const long released = rss_loaded - rss_closed;
  std::fprintf(stderr,
               "[mem-probe] RSS before=%ld KiB, after load=%ld KiB (+%ld), "
               "after close=%ld KiB (released %ld KiB)\n",
               rss_before,
               rss_loaded,
               gained,
               rss_closed,
               released);

#if !defined(NEKO_TEST_SANITIZED)
  if (gained > 20 * 1024) {
    EXPECT_GE(released, gained / 4) << "closing the tab must return freed pages to the OS";
  }
#endif
  std::filesystem::remove(page);
#endif
}

// Page zoom: the keyboard shortcuts step the active tab's factor, the toolbar
// indicator follows it, and clicking the indicator resets to 100%.  The layout
// really changes: the 120 CSS-pixel box measures 110% of that after Ctrl+=.
TEST(UiSmokeTest, ZoomShortcutsAndIndicator)
{
  TempProfile tp;
  const std::string html_file = tp.path() + "/zoom.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
                  html_file,
                  "<html><body style=\"margin:0\"><div id=box "
                  "style=\"background:#ff0000;width:120px;height:60px\"></div></body></html>")
                  .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();
  ASSERT_TRUE(WaitFor([&] {
    return worker.SnapshotActiveTab().content_type == neko::browser::ContentType::kHtml;
  }));

  // The indicator starts at 100%.
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "100%"; }));

  // Ctrl+= (and Ctrl++, the shifted form) step up; Ctrl+- steps down.
  SendKey(&window, Qt::Key_Equal, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "110%"; }));
  SendKey(&window, Qt::Key_Plus, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "125%"; }));
  SendKey(&window, Qt::Key_Minus, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "110%"; }));

  // The zoom reached the page: 1.1 x 120 CSS px = 132 device px.
  const neko::browser::TabSnapshot zoomed = worker.SnapshotActiveTab();
  EXPECT_FLOAT_EQ(zoomed.zoom, 1.1F);
  ASSERT_TRUE(zoomed.page != nullptr);
  neko::dom::Element* box = neko::dom::QuerySelector(*zoomed.page->document(), "#box");
  ASSERT_NE(box, nullptr);
  const auto geometry = zoomed.page->ElementBoxGeometry(*box);
  ASSERT_TRUE(geometry.has_value());
  EXPECT_NEAR(geometry->width, 132.0F, 2.0F);

  // Clicking the indicator resets to 100% (Ctrl+0 does the same).
  QTest::mouseClick(window.ZoomIndicator(), Qt::LeftButton);
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "100%"; }));
  SendKey(&window, Qt::Key_Equal, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "110%"; }));
  SendKey(&window, Qt::Key_0, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "100%"; }));
  EXPECT_FLOAT_EQ(worker.SnapshotActiveTab().zoom, 1.0F);
}

// Find-in-page (Ctrl+F): the bar opens focused, the query runs against the
// laid-out page, Enter / Shift+Enter step the matches (the counter follows,
// wrapping around), a query without matches reports 0/0, and Esc clears the
// session and closes the bar.
TEST(UiSmokeTest, FindBarFindsAndStepsMatches)
{
  TempProfile tp;
  const std::string html_file = tp.path() + "/find.html";
  ASSERT_TRUE(
      neko::storage::WriteFileAtomic(html_file,
                                     "<html><head><title>Find Test</title></head>"
                                     "<body><p>alpha beta</p><p>gamma alpha</p></body></html>")
          .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();
  ASSERT_TRUE(WaitFor([&] {
    return worker.SnapshotActiveTab().content_type == neko::browser::ContentType::kHtml;
  }));

  // Ctrl+F opens the bar (hidden by default).
  ASSERT_NE(window.FindBar(), nullptr);
  EXPECT_FALSE(window.FindBarShowing());
  SendKey(&window, Qt::Key_F, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.FindBarShowing(); }));

  // Typing searches: two occurrences of "alpha", the first one is current.
  window.FindInput()->setText(QStringLiteral("alpha"));
  ASSERT_TRUE(WaitFor([&] { return window.FindStatus()->text() == "1/2"; }));
  EXPECT_EQ(worker.SnapshotActiveTab().find_match_count, 2);

  // Enter steps forward and wraps; Shift+Enter steps backwards.
  SendKey(window.FindInput(), Qt::Key_Return);
  ASSERT_TRUE(WaitFor([&] { return window.FindStatus()->text() == "2/2"; }));
  SendKey(window.FindInput(), Qt::Key_Return);
  ASSERT_TRUE(WaitFor([&] { return window.FindStatus()->text() == "1/2"; }));
  SendKey(window.FindInput(), Qt::Key_Return, Qt::ShiftModifier);
  ASSERT_TRUE(WaitFor([&] { return window.FindStatus()->text() == "2/2"; }));

  // A query with no matches is reported honestly.
  window.FindInput()->clear();
  window.FindInput()->setText(QStringLiteral("absent"));
  ASSERT_TRUE(WaitFor([&] { return window.FindStatus()->text() == "0/0"; }));
  EXPECT_EQ(worker.SnapshotActiveTab().find_match_count, 0);

  // Esc closes the bar and drops the find session.
  SendKey(window.FindInput(), Qt::Key_Escape);
  ASSERT_TRUE(WaitFor([&] { return !window.FindBarShowing(); }));
  ASSERT_TRUE(WaitFor([&] { return worker.SnapshotActiveTab().find_query.empty(); }));
}

TEST(UiSmokeTest, HoverDoesNotResetScroll)
{
  TempProfile tp;
  std::string html = "<html><head><title>Scroll</title>"
                     "<style>p:hover { color: red; }</style></head><body>";
  for (int i = 0; i < 80; ++i) {
    html += "<p>paragraph number " + std::to_string(i) + "</p>";
  }
  html += "</body></html>";
  const std::string html_file = tp.path() + "/scroll.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  // Wait until the page is laid out (so the scroll range is set).
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.content_type == neko::browser::ContentType::kHtml && snap.page != nullptr &&
           snap.page->layout_root() != nullptr;
  }));

  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  // The viewport frame (and therefore the scroll range) is produced by the
  // worker's pump (ADR 0020), so wait for it instead of asserting immediately
  // on the layout being ready.
  ASSERT_TRUE(WaitFor([&] { return view->verticalScrollBar()->maximum() > 0; }));

  view->verticalScrollBar()->setValue(200);
  ASSERT_EQ(view->verticalScrollBar()->value(), 200);

  // Hover over the page (a MouseMove into the viewport); this changes the
  // hovered element and re-runs the cascade/layout.
  QTest::mouseMove(view->viewport(), QPoint(static_cast<int>(100), static_cast<int>(100)));
  QCoreApplication::processEvents();

  // A script-pump / navigation refresh must not treat the hover-induced work
  // as a fresh load and reset the scroll to the top.
  view->Refresh();
  QCoreApplication::processEvents();

  EXPECT_EQ(view->verticalScrollBar()->value(), 200);
}

TEST(UiSmokeTest, ClickRunsPageClickListener)
{
  TempProfile tp;
  const std::string html =
      "<html><head><title>Click</title></head>"
      "<body style=\"margin:0\">"
      "<button id=\"btn\" style=\"width:200px;height:40px;margin:10px\">Click</button>"
      "<span id=\"status\">no</span>"
      "<script>"
      "document.getElementById('btn').addEventListener('click', function(){"
      "  document.getElementById('status').textContent = 'yes'; });"
      "</script></body></html>";
  const std::string html_file = tp.path() + "/click.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.page != nullptr && snap.page->layout_root() != nullptr;
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);

  // Click the button's center: body margin 0, button margin 10 + 200x40 →
  // the button spans roughly (10,10)..(210,50); (100,30) hits it.
  QTest::mousePress(view->viewport(),
                    Qt::LeftButton,
                    Qt::NoModifier,
                    QPoint(static_cast<int>(100), static_cast<int>(30)));
  QTest::mouseRelease(view->viewport(),
                      Qt::LeftButton,
                      Qt::NoModifier,
                      QPoint(static_cast<int>(100), static_cast<int>(30)));

  // The click is dispatched on the worker thread; wait until the page script
  // observed it and updated #status.
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* status = neko::dom::QuerySelector(*snap.page->document(), "#status");
    return status != nullptr && status->TextContent() == "yes";
  }));
}

// Finds the first laid-out text run belonging to |target| and returns its
// top-left point (document coordinates, before scroll).
bool FindElementRunPoint(const neko::layout::LayoutBox& box,
                         const neko::dom::Element* target,
                         float& x,
                         float& y)
{
  for (const neko::layout::Line& line : box.lines) {
    for (const neko::layout::TextRun& run : line.runs) {
      if (run.element == target) {
        x = run.x + 1.0f;
        y = run.y + 1.0f;
        return true;
      }
    }
    for (const neko::layout::InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr && FindElementRunPoint(*ib.block_box, target, x, y)) {
        return true;
      }
    }
  }
  for (const auto& child : box.children) {
    if (FindElementRunPoint(*child, target, x, y)) {
      return true;
    }
  }
  for (const auto& f : box.floats) {
    if (FindElementRunPoint(*f, target, x, y)) {
      return true;
    }
  }
  return false;
}

TEST(UiSmokeTest, ClickInputAndTypeUpdatesValue)
{
  TempProfile tp;
  const std::string html = "<html><head><title>Input</title></head>"
                           "<body style=\"margin:0\">"
                           "<input id=\"q\" value=\"hi\" style=\"margin:10px\">"
                           "</body></html>";
  const std::string html_file = tp.path() + "/input.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    if (snap.page == nullptr || snap.page->layout_root() == nullptr ||
        snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
    if (input == nullptr) {
      return false;
    }
    float x = 0;
    float y = 0;
    return FindElementRunPoint(*snap.page->layout_root(), input, x, y);
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  // The WebView's cached snapshot only refreshes on the main window's timer;
  // sync it so click/key handling sees the loaded page.
  view->Refresh();

  // Click the input's value text: it gains focus and keyboard focus moves to
  // the WebView (address bar clears above, but the click steals focus too).
  {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    ASSERT_NE(snap.page->document(), nullptr);
    neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
    ASSERT_NE(input, nullptr);
    float x = 0;
    float y = 0;
    ASSERT_TRUE(FindElementRunPoint(*snap.page->layout_root(), input, x, y));
    QTest::mousePress(view->viewport(),
                      Qt::LeftButton,
                      Qt::NoModifier,
                      QPoint(static_cast<int>(x), static_cast<int>(y)));
    QTest::mouseRelease(view->viewport(),
                        Qt::LeftButton,
                        Qt::NoModifier,
                        QPoint(static_cast<int>(x), static_cast<int>(y)));
  }
  // The focused control is published on the page (read by caret painting).
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.page != nullptr && snap.page->FocusedElement() != nullptr;
  }));

  // Type a character; the WebView now owns keyboard focus, so the keystroke is
  // forwarded to the focused input (keydown inserts, keyup only dispatches).
  {
    QKeyEvent down(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QApplication::sendEvent(view, &down);
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
    QApplication::sendEvent(view, &up);
  }

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
    return input != nullptr && input->GetAttribute("value").value_or("") == "hia";
  }));
}

TEST(UiSmokeTest, TextInputPreservesPrintableUnicodeCharacters)
{
  TempProfile tp;
  const std::string html = "<html><body style=\"margin:0\">"
                           "<input id=\"q\" value=\"x\" style=\"margin:10px\">"
                           "</body></html>";
  const std::string html_file = tp.path() + "/unicode-input.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.page != nullptr && snap.page->layout_root() != nullptr &&
           snap.page->document() != nullptr &&
           neko::dom::QuerySelector(*snap.page->document(), "#q") != nullptr;
  }));

  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  view->Refresh();
  const auto snap = worker.SnapshotActiveTab();
  auto* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
  ASSERT_NE(input, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*snap.page->layout_root(), input, x, y));
  QTest::mouseClick(view->viewport(),
                    Qt::LeftButton,
                    Qt::NoModifier,
                    QPoint(static_cast<int>(x), static_cast<int>(y)));

  QKeyEvent at_down(QEvent::KeyPress, Qt::Key_At, Qt::ShiftModifier, "@");
  QApplication::sendEvent(view, &at_down);
  QKeyEvent unicode_down(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromUtf8("中"));
  QApplication::sendEvent(view, &unicode_down);

  ASSERT_TRUE(WaitFor([&] {
    const auto current = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(current.page);
    if (current.page == nullptr || current.page->document() == nullptr) {
      return false;
    }
    auto* current_input = neko::dom::QuerySelector(*current.page->document(), "#q");
    return current_input != nullptr && current_input->GetAttribute("value").value_or("") == "x@中";
  }));
}

// Returns the caret point of |target|: the end of its first text run
// (document coordinates, before scroll).
bool FindCaretPoint(const neko::layout::LayoutBox& box,
                    const neko::dom::Element* target,
                    float& x,
                    float& y,
                    float& h)
{
  for (const neko::layout::Line& line : box.lines) {
    for (const neko::layout::TextRun& run : line.runs) {
      if (run.element == target) {
        x = run.x + run.width;
        y = run.y;
        h = line.height;
        return true;
      }
    }
    for (const neko::layout::InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr && FindCaretPoint(*ib.block_box, target, x, y, h)) {
        return true;
      }
    }
  }
  for (const auto& child : box.children) {
    if (FindCaretPoint(*child, target, x, y, h)) {
      return true;
    }
  }
  for (const auto& f : box.floats) {
    if (FindCaretPoint(*f, target, x, y, h)) {
      return true;
    }
  }
  return false;
}

TEST(UiSmokeTest, FocusedInputDrawsCaret)
{
  TempProfile tp;
  const std::string html = "<html><head><title>I</title></head>"
                           "<body style=\"margin:0\">"
                           "<input id=\"q\" value=\"ab\" style=\"margin:10px\">"
                           "</body></html>";
  const std::string html_file = tp.path() + "/caret.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    if (snap.page == nullptr || snap.page->layout_root() == nullptr ||
        snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
    if (input == nullptr) {
      return false;
    }
    float x = 0;
    float y = 0;
    float h = 0;
    return FindCaretPoint(*snap.page->layout_root(), input, x, y, h);
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  view->Refresh();

  // Click the input's text to focus it.
  {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
    float x = 0;
    float y = 0;
    float h = 0;
    ASSERT_TRUE(FindCaretPoint(*snap.page->layout_root(), input, x, y, h));
    QTest::mousePress(view->viewport(),
                      Qt::LeftButton,
                      Qt::NoModifier,
                      QPoint(static_cast<int>(x - 2.0f), static_cast<int>(y)));
    QTest::mouseRelease(view->viewport(),
                        Qt::LeftButton,
                        Qt::NoModifier,
                        QPoint(static_cast<int>(x - 2.0f), static_cast<int>(y)));
  }
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.page != nullptr && snap.page->FocusedElement() != nullptr;
  }));

  // Grab the viewport across several blink intervals: the caret must be
  // present in some frames and absent in others (blinking).
  const auto snap = worker.SnapshotActiveTab();
  neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
  ASSERT_NE(input, nullptr);
  float cx = 0;
  float cy = 0;
  float ch = 0;
  ASSERT_TRUE(FindCaretPoint(*snap.page->layout_root(), input, cx, cy, ch));
  bool saw_caret = false;
  bool saw_no_caret = false;
  for (int i = 0; i < 5; ++i) {
    const QImage img = view->viewport()->grab().toImage();
    bool has = false;
    for (int y = std::max(0, static_cast<int>(cy));
         y < static_cast<int>(cy + ch) && y < img.height();
         ++y) {
      if (static_cast<int>(cx) < img.width()) {
        const QColor c = img.pixelColor(static_cast<int>(cx), y);
        if (c.red() < 100 && c.green() < 100 && c.blue() < 100) {
          has = true;
          break;
        }
      }
    }
    (has ? saw_caret : saw_no_caret) = true;
    if (saw_caret && saw_no_caret) {
      break;
    }
    // Let the 500 ms blink timer fire before the next frame.
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(560));
  }
  EXPECT_TRUE(saw_caret);
  EXPECT_TRUE(saw_no_caret);

  // The caret follows the value: typing moves it to the new end of the text.
  {
    QKeyEvent down(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier);
    QApplication::sendEvent(view, &down);
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_C, Qt::NoModifier);
    QApplication::sendEvent(view, &up);
  }
  ASSERT_TRUE(WaitFor([&] {
    const auto snap2 = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    if (snap2.page == nullptr || snap2.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* q = neko::dom::QuerySelector(*snap2.page->document(), "#q");
    return q != nullptr && q->GetAttribute("value").value_or("") == "abc";
  }));
  float cx2 = 0;
  float cy2 = 0;
  float ch2 = 0;
  {
    const auto snap2 = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    neko::dom::Element* q = neko::dom::QuerySelector(*snap2.page->document(), "#q");
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(FindCaretPoint(*snap2.page->layout_root(), q, cx2, cy2, ch2));
  }
  EXPECT_GT(cx2, cx);
  // The caret must still blink at its new position.
  bool saw_caret2 = false;
  bool saw_no_caret2 = false;
  for (int i = 0; i < 5; ++i) {
    const QImage img = view->viewport()->grab().toImage();
    bool has = false;
    for (int y = std::max(0, static_cast<int>(cy2));
         y < static_cast<int>(cy2 + ch2) && y < img.height();
         ++y) {
      if (static_cast<int>(cx2) < img.width()) {
        const QColor c = img.pixelColor(static_cast<int>(cx2), y);
        if (c.red() < 100 && c.green() < 100 && c.blue() < 100) {
          has = true;
          break;
        }
      }
    }
    (has ? saw_caret2 : saw_no_caret2) = true;
    if (saw_caret2 && saw_no_caret2) {
      break;
    }
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(560));
  }
  EXPECT_TRUE(saw_caret2);
  EXPECT_TRUE(saw_no_caret2);
}

TEST(UiSmokeTest, WheelFiresPageWheelEvent)
{
  TempProfile tp;
  const std::string html = "<html><head><title>W</title></head>"
                           "<body style=\"margin:0\"><div style=\"height:2000px\">tall</div>"
                           "<script>"
                           "document.body.onwheel = function(e){"
                           "  document.body.setAttribute('data-d', e.deltaY);"
                           "};"
                           "</script></body></html>";
  const std::string html_file = tp.path() + "/wheel.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.page != nullptr && snap.page->layout_root() != nullptr;
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  view->Refresh();

  // One wheel notch: deltaY = the line step chosen by the view.
  QWheelEvent wheel(QPointF(100, 100),
                    QPointF(100, 100),
                    QPoint(0, 0),
                    QPoint(0, -120),
                    Qt::NoButton,
                    Qt::NoModifier,
                    Qt::NoScrollPhase,
                    /*inverted=*/false);
  QApplication::sendEvent(view->viewport(), &wheel);

  // The page's onwheel handler observed a non-zero vertical delta.
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* body = neko::dom::QuerySelector(*snap.page->document(), "body");
    return body != nullptr && body->GetAttribute("data-d").has_value() &&
           std::stod(std::string(body->GetAttribute("data-d").value())) != 0;
  }));
}

TEST(UiSmokeTest, HoverOverElementFiresPageMouseOver)
{
  TempProfile tp;
  const std::string html = "<html><head><title>H</title></head>"
                           "<body style=\"margin:0\">"
                           "<div id=\"box\" style=\"width:200px;height:80px\">hover me</div>"
                           "<script>"
                           "var box = document.getElementById('box');"
                           "box.onmouseover = function(){ box.setAttribute('data-h', '1'); };"
                           "box.onmouseout = function(){ box.setAttribute('data-h', '0'); };"
                           "</script></body></html>";
  const std::string html_file = tp.path() + "/hover.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.page != nullptr && snap.page->layout_root() != nullptr;
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  view->Refresh();

  // Move over the box (top-left of the page) -> page onmouseover.  The event
  // is sent directly because QTest::mouseMove goes through the platform
  // integration, which the offscreen plugin may coalesce when the global
  // cursor position is unchanged.
  {
    const QPointF local(50, 10);
    QMouseEvent move(QEvent::MouseMove,
                     local,
                     view->viewport()->mapToGlobal(local),
                     Qt::NoButton,
                     Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &move);
  }
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* box = neko::dom::QuerySelector(*snap.page->document(), "#box");
    return box != nullptr && box->GetAttribute("data-h").value_or("") == "1";
  }));

  // Leave the viewport -> onmouseout.
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(view->viewport(), &leave);
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* box = neko::dom::QuerySelector(*snap.page->document(), "#box");
    return box != nullptr && box->GetAttribute("data-h").value_or("") == "0";
  }));
}

TEST(UiSmokeTest, ClickLinkNavigates)
{
  TempProfile tp;
  const std::string html = "<html><head><title>Link</title></head>"
                           "<body style=\"margin:0\"><a href=\"/nav\" id=\"lk\">go</a></body>";
  const std::string html_file = tp.path() + "/link.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.page != nullptr && snap.page->layout_root() != nullptr;
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);

  const auto snap = worker.SnapshotActiveTab();
  neko::dom::Element* link = neko::dom::QuerySelector(*snap.page->document(), "#lk");
  ASSERT_NE(link, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*snap.page->layout_root(), link, x, y));

  QTest::mousePress(view->viewport(),
                    Qt::LeftButton,
                    Qt::NoModifier,
                    QPoint(static_cast<int>(x), static_cast<int>(y)));
  QTest::mouseRelease(view->viewport(),
                      Qt::LeftButton,
                      Qt::NoModifier,
                      QPoint(static_cast<int>(x), static_cast<int>(y)));

  // The default action navigates the link to /nav.
  ASSERT_TRUE(WaitFor([&] {
    const auto s = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(s.page);
    return s.url.find("/nav") != std::string::npos;
  }));
}

TEST(UiSmokeTest, HoverLinkShowsPointingHand)
{
  TempProfile tp;
  const std::string html = "<html><head><title>Hover</title></head>"
                           "<body style=\"margin:0\"><a href=\"/x\" id=\"lk\">go</a></body>";
  const std::string html_file = tp.path() + "/hover.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file, html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    const PageDomReadLock page_guard(snap.page);
    return snap.page != nullptr && snap.page->layout_root() != nullptr;
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  // Sync the view's copy of the snapshot: HandleHover hit-tests against it and
  // bails out while it is still empty (the periodic refresh is not guaranteed
  // to have run yet).
  view->Refresh();

  const auto snap = worker.SnapshotActiveTab();
  neko::dom::Element* link = neko::dom::QuerySelector(*snap.page->document(), "#lk");
  ASSERT_NE(link, nullptr);
  float x = 0;
  float y = 0;
  ASSERT_TRUE(FindElementRunPoint(*snap.page->layout_root(), link, x, y));

  // Hovering the hyperlink switches the pointer to a pointing hand.  The event
  // is sent directly (QTest::mouseMove goes through the platform integration,
  // which the offscreen plugin may coalesce when the global cursor position is
  // unchanged, making the assertion racy).
  auto send_move = [&](int px, int py) {
    const QPointF local(px, py);
    QMouseEvent move(QEvent::MouseMove,
                     local,
                     view->viewport()->mapToGlobal(local),
                     Qt::NoButton,
                     Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &move);
  };
  send_move(static_cast<int>(x), static_cast<int>(y));
  EXPECT_TRUE(
      WaitFor([&] { return view->viewport()->cursor().shape() == Qt::PointingHandCursor; }));

  // Hovering elsewhere restores the arrow.
  send_move(500, 400);
  EXPECT_TRUE(WaitFor([&] { return view->viewport()->cursor().shape() == Qt::ArrowCursor; }));
}

// A page script calling window.scrollTo drives the GUI's scroll bar: the
// worker bumps scroll_request_id and the WebView's Refresh applies the
// requested offset to its vertical scroll bar.
TEST(UiSmokeTest, ScriptScrollMovesViewportScrollBar)
{
  TempProfile tp;
  const std::string html_file = tp.path() + "/scroll.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file,
                                             "<html><body style=\"height:3000px\">"
                                             "<script>window.scrollTo(0, 150);</script>"
                                             "<p>tall content</p></body></html>")
                  .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();

  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  // The script's requested scroll lands on the view's scroll bar once the
  // page has been laid out (scroll range set) and the latch applied.
  ASSERT_TRUE(WaitFor([&] {
    neko::ui::WebView* view = window.ActiveView();
    return view != nullptr && view->verticalScrollBar()->maximum() > 0 &&
           view->verticalScrollBar()->value() == 150;
  }));
  // The content is tall enough that 150 is a real (non-clamped) offset.
  EXPECT_GT(window.ActiveView()->verticalScrollBar()->maximum(), 150);
}

// ---------------------------------------------------------------------------
// Renderer-process mode (ADR 0016 M2): the GUI paints the frames the child
// process produced, and input is forwarded to it.
// ---------------------------------------------------------------------------

namespace {

neko::browser::RendererOptions RendererModeOptions()
{
  neko::browser::RendererOptions renderer;
  renderer.enabled = true;
  renderer.executable = NEKO_BROWSER_BIN;
  return renderer;
}

// True when the grabbed viewport contains a dark pixel, i.e. the page's text
// was actually painted (a blank view is all white).
bool ViewportHasDarkPixel(neko::ui::WebView* view)
{
  if (view == nullptr) {
    return false;
  }
  const QImage image = view->viewport()->grab().toImage();
  for (int y = 0; y < image.height(); y += 2) {
    for (int x = 0; x < image.width(); x += 2) {
      const QRgb pixel = image.pixel(x, y);
      if (qRed(pixel) < 160 && qGreen(pixel) < 160 && qBlue(pixel) < 160) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

TEST(UiSmokeTest, RendererProcessModePaintsChildFrames)
{
  TempProfile tp;
  const std::string html_file = tp.path() + "/remote.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file,
                                             "<html><head><title>Remote UI</title></head>"
                                             "<body><h1>Hello Remote</h1></body></html>")
                  .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()), nullptr, RendererModeOptions());
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();

  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  // The title is reported by the child through the session.
  ASSERT_TRUE(WaitFor([&] {
    for (int i = 0; i < window.TabBarWidget()->count(); ++i) {
      if (window.TabBarWidget()->tabText(i).contains("Remote UI")) {
        return true;
      }
    }
    return false;
  }));

  // The frame is painted: the heading's dark text must appear in the viewport.
  neko::ui::WebView* view = window.ActiveView();
  ASSERT_NE(view, nullptr);
  EXPECT_TRUE(WaitFor([&] { return ViewportHasDarkPixel(view); }, 10000));

  // The document lives in the child: no in-process page is published.
  const neko::browser::TabSnapshot snapshot = worker.SnapshotActiveTab();
  EXPECT_TRUE(snapshot.remote);
  EXPECT_EQ(snapshot.page, nullptr);
  ASSERT_NE(snapshot.remote_frame, nullptr);
}

// Zoom in renderer-process mode: the shortcut travels browser → child over the
// session protocol, the indicator tracks the applied factor, and the child's
// reported content height grows with the scale (the GUI scroll bar follows).
TEST(UiSmokeTest, RendererProcessModeZoomReachesTheChild)
{
  TempProfile tp;
  const std::string html_file = tp.path() + "/remote_zoom.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(html_file,
                                             "<html><head><title>Remote Zoom</title></head>"
                                             "<body style=\"margin:0\"><h1>Zoomed</h1></body>"
                                             "</html>")
                  .has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()), nullptr, RendererModeOptions());
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();
  worker.NavigateActive(QString::fromStdString(html_file));
  window.AddressBar()->clearFocus();

  ASSERT_TRUE(WaitFor([&] { return worker.SnapshotActiveTab().remote; }, 10000));
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "100%"; }));
  // The height arrives with the child's first frame.
  ASSERT_TRUE(
      WaitFor([&] { return worker.SnapshotActiveTab().remote_content_height > 0.0F; }, 10000));
  const float height_100 = worker.SnapshotActiveTab().remote_content_height;
  ASSERT_GT(height_100, 0.0F);

  SendKey(&window, Qt::Key_Equal, Qt::ControlModifier);
  ASSERT_TRUE(WaitFor([&] { return window.ZoomIndicator()->text() == "110%"; }, 10000));
  // The indicator tracks the browser-side zoom immediately, but the child
  // re-lays out asynchronously and reports the new content height a round trip
  // later: wait for it instead of racing the first snapshot.
  const auto height_grew = [&] {
    return worker.SnapshotActiveTab().remote_content_height > height_100 * 1.05F;
  };
  ASSERT_TRUE(WaitFor(height_grew, 10000));
  const neko::browser::TabSnapshot zoomed = worker.SnapshotActiveTab();
  EXPECT_FLOAT_EQ(zoomed.zoom, 1.1F);
  EXPECT_TRUE(zoomed.remote);
  EXPECT_NEAR(zoomed.remote_content_height, height_100 * 1.1F, 1.0F);
}

TEST(UiSmokeTest, RendererProcessModeClickAndHoverReachTheChild)
{
  TempProfile tp;
  const std::string first = tp.path() + "/first.html";
  const std::string second = tp.path() + "/second.html";
  ASSERT_TRUE(
      neko::storage::WriteFileAtomic(
          second, "<html><head><title>Second Remote</title></head><body>second</body></html>")
          .has_value());
  const std::string first_html = "<html><head><title>First Remote</title></head>"
                                 "<body><a id=\"next\" href=\"file://" +
                                 second + "\">go</a></body></html>";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(first, first_html).has_value());

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()), nullptr, RendererModeOptions());
  neko::ui::MainWindow window(&worker);
  window.resize(800, 600);
  window.show();

  worker.NavigateActive(QString::fromStdString(first));
  window.AddressBar()->clearFocus();
  ASSERT_TRUE(WaitFor([&] {
    for (int i = 0; i < window.TabBarWidget()->count(); ++i) {
      if (window.TabBarWidget()->tabText(i).contains("First Remote")) {
        return true;
      }
    }
    return false;
  }));

  neko::ui::WebView* view = window.ActiveView();
  ASSERT_NE(view, nullptr);
  // Click/hover coordinates from the same engine (the frame is 1:1 with
  // document pixels at scroll offset 0).
  neko::renderer::Page probe;
  ASSERT_TRUE(probe.LoadHtml(first_html).has_value());
  probe.Layout(static_cast<float>(view->viewport()->width()),
               static_cast<float>(view->viewport()->height()));
  neko::dom::Element* link = neko::dom::QuerySelector(*probe.document(), "#next");
  ASSERT_NE(link, nullptr);
  const auto geometry = probe.ElementBoxGeometry(*link);
  ASSERT_TRUE(geometry.has_value());
  const QPoint point(static_cast<int>(geometry->x + geometry->width / 2.0f),
                     static_cast<int>(geometry->y + geometry->height / 2.0f));

  // Hovering the link: the child reports the target and the cursor follows.
  {
    const QPointF local(point);
    QMouseEvent move(QEvent::MouseMove,
                     local,
                     view->viewport()->mapToGlobal(local),
                     Qt::NoButton,
                     Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &move);
  }
  EXPECT_TRUE(
      WaitFor([&] { return view->viewport()->cursor().shape() == Qt::PointingHandCursor; }));

  // Clicking it: the child performs the navigation and reports it back, the
  // browser re-runs it (file read) and the tab lands on the second page.
  QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
  QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
  EXPECT_TRUE(WaitFor(
      [&] {
        for (int i = 0; i < window.TabBarWidget()->count(); ++i) {
          if (window.TabBarWidget()->tabText(i).contains("Second Remote")) {
            return true;
          }
        }
        return false;
      },
      10000));
}

TEST(UiSmokeTest, SavedLoginsSettingsListShowsAndDeletes)
{
  TempProfile tp;
  // Seed the encrypted password store directly: this UI test has no HTTP
  // server to produce a real form submission, and the settings list only
  // needs a stored login to display.
  {
    neko::storage::PasswordStore store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    store.Add("https://example.com", "alice", "s3cret");
    const auto saved = store.Save();
    ASSERT_TRUE(saved.has_value()) << (saved.has_value() ? "" : saved.error().message());
  }

  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(1024, 768);
  window.show();
  for (int i = 0; i < 10; ++i) {
    QCoreApplication::processEvents();
  }

  QListWidget* list = window.PasswordListWidget();
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1);
  EXPECT_TRUE(list->item(0)->text().contains(QStringLiteral("example.com")));
  EXPECT_TRUE(list->item(0)->text().contains(QStringLiteral("alice")));
  // The password itself must never appear in the UI.
  EXPECT_FALSE(list->item(0)->text().contains(QStringLiteral("s3cret")));

  // The save prompt stays hidden while no login is pending.
  ASSERT_NE(window.PasswordBarWidget(), nullptr);
  EXPECT_FALSE(window.PasswordBarWidget()->isVisible());

  // Delete the selected login: the removal round-trips through the worker and
  // the list refreshes to empty.
  list->setCurrentRow(0);
  auto* delete_button = window.findChild<QPushButton*>(QStringLiteral("passwordDeleteButton"));
  ASSERT_NE(delete_button, nullptr);
  delete_button->click();
  ASSERT_TRUE(WaitFor([&] { return list->count() == 0; }));
  EXPECT_TRUE(worker.SavedLogins().empty());
  // And the removal reaches the disk (a fresh store sees no records).
  {
    neko::storage::PasswordStore store(tp.path());
    ASSERT_TRUE(store.Load().has_value());
    EXPECT_TRUE(store.All().empty());
  }
}

// ---------------------------------------------------------------------------
// Internationalization
// ---------------------------------------------------------------------------

TEST(UiSmokeTest, TranslationCatalogsLoadAndTranslate)
{
  // Every shipped language must have a loadable catalog carrying real
  // translations (spot-checked through the MainWindow context).
  neko::ui::i18n::EnsureTranslationResources();
  const std::vector<neko::ui::i18n::Language>& languages = neko::ui::i18n::SupportedLanguages();
  EXPECT_GE(languages.size(), 7u);
  for (const neko::ui::i18n::Language& language : languages) {
    const std::string id(language.id);
    if (id == "en") {
      EXPECT_TRUE(neko::ui::i18n::CatalogPath(id).empty());
      continue;
    }
    QTranslator translator;
    const std::string path = neko::ui::i18n::CatalogPath(id);
    ASSERT_TRUE(translator.load(QString::fromStdString(path))) << path;
    QCoreApplication::installTranslator(&translator);
    const QString settings = QCoreApplication::translate("neko::ui::MainWindow", "Settings");
    QCoreApplication::removeTranslator(&translator);
    EXPECT_NE(settings, QStringLiteral("Settings")) << id;
  }
}

TEST(UiSmokeTest, ResolveLanguageIdFollowsPreferenceOrSystem)
{
  using neko::ui::i18n::IsRightToLeft;
  using neko::ui::i18n::IsSupportedLanguage;
  using neko::ui::i18n::ResolveLanguageId;
  EXPECT_EQ(ResolveLanguageId("ja"), "ja");
  EXPECT_EQ(ResolveLanguageId("zh_TW"), "zh_TW");
  EXPECT_EQ(ResolveLanguageId("ar"), "ar");
  EXPECT_EQ(ResolveLanguageId("en"), "en");
  // Unknown (or empty) preferences follow the system locale; whatever this
  // machine reports, the result must be a selectable language.
  const std::string from_system = ResolveLanguageId("klingon");
  EXPECT_TRUE(IsSupportedLanguage(from_system));
  EXPECT_EQ(ResolveLanguageId(""), from_system);
  EXPECT_TRUE(IsRightToLeft("ar"));
  EXPECT_FALSE(IsRightToLeft("ru"));
  EXPECT_FALSE(IsRightToLeft("en"));
}

TEST(UiSmokeTest, ApplyLanguageInstallsTranslatorAndFlipsDirection)
{
  // Arabic: the application flips to RTL and the UI translator is installed.
  const std::string effective = neko::ui::i18n::ApplyLanguage(*qApp, "ar");
  EXPECT_EQ(effective, "ar");
  EXPECT_EQ(QApplication::layoutDirection(), Qt::RightToLeft);
  EXPECT_NE(QCoreApplication::translate("neko::ui::MainWindow", "Settings"),
            QStringLiteral("Settings"));

  // Switching back to English (the source language) removes the translator
  // and restores the default direction, so the rest of the suite runs on the
  // untranslated source strings.
  EXPECT_EQ(neko::ui::i18n::ApplyLanguage(*qApp, "en"), "en");
  EXPECT_EQ(QApplication::layoutDirection(), Qt::LeftToRight);
  EXPECT_EQ(QCoreApplication::translate("neko::ui::MainWindow", "Settings"),
            QStringLiteral("Settings"));
}

TEST(UiSmokeTest, LanguageSelectionPersists)
{
  TempProfile tp;
  neko::ui::BrowserWorker worker(QString::fromStdString(tp.path()));
  neko::ui::MainWindow window(&worker);
  window.resize(1024, 768);
  window.show();
  for (int i = 0; i < 10; ++i) {
    QCoreApplication::processEvents();
  }

  QComboBox* combo = window.LanguageComboWidget();
  ASSERT_NE(combo, nullptr);
  // "System default" plus every shipped language.
  EXPECT_EQ(combo->count(), static_cast<int>(neko::ui::i18n::SupportedLanguages().size()) + 1);
  EXPECT_EQ(combo->itemData(0).toString(), QString());

  // Selecting a language stores the preference (it is applied at startup).
  const int japanese = combo->findData(QStringLiteral("ja"));
  ASSERT_GE(japanese, 0);
  combo->setCurrentIndex(japanese);
  EXPECT_TRUE(WaitFor([&] {
    for (const auto& [key, value] : worker.SnapshotPreferences()) {
      if (key == "language") {
        return value == "ja";
      }
    }
    return false;
  }));

  // The settings sync keeps the widget on the stored value across refreshes.
  QCoreApplication::processEvents();
  EXPECT_EQ(combo->currentData().toString(), QStringLiteral("ja"));
}

} // namespace
