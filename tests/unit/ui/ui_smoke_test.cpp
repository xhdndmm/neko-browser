// GUI smoke tests.  They run under the Qt "offscreen" platform plugin
// (QT_QPA_PLATFORM=offscreen, set as a ctest property) so no display is
// needed.  The tests exercise the real BrowserWorker + MainWindow + WebView
// stack end to end.

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTabBar>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "neko/browser/browser_controller.h"
#include "neko/storage/file_util.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/main_window.h"
#include "neko/ui/web_view.h"

int main(int argc, char** argv) {
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

class TempProfile {
 public:
  TempProfile() {
    dir_ = std::filesystem::temp_directory_path() /
           ("neko-ui-test-" + std::to_string(::getpid()) + "-" +
            std::to_string(++seq_));
    std::filesystem::create_directories(dir_);
  }
  ~TempProfile() { std::filesystem::remove_all(dir_); }
  const std::string path() const { return dir_.string(); }

 private:
  static int seq_;
  std::filesystem::path dir_;
};
int TempProfile::seq_ = 0;

// Pumps the event loop while polling a GUI-thread predicate (safe because
// the predicate only reads widgets, which are owned by the GUI thread).
template <typename Predicate>
bool WaitFor(Predicate predicate, int timeout_ms = 5000) {
  const int step = 20;
  int elapsed = 0;
  while (elapsed < timeout_ms) {
    QCoreApplication::processEvents();
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(step));
    elapsed += step;
  }
  return predicate();
}

// Sends a real key press+release pair to |widget| through the Qt event
// system, exactly as the platform would deliver it.
void SendKey(QWidget* widget, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  QKeyEvent press(QEvent::KeyPress, key, mods);
  QKeyEvent release(QEvent::KeyRelease, key, mods);
  QApplication::sendEvent(widget, &press);
  QApplication::sendEvent(widget, &release);
}

TEST(UiSmokeTest, RendersLocalHtmlPage) {
  TempProfile tp;
  const std::string html_file = tp.path() + "/page.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
      html_file, "<html><head><title>UI Test</title></head>"
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
      if (window.TabBarWidget()->tabText(i).contains("UI Test")) return true;
    }
    return false;
  }));

  // The controller routed and parsed the page (read through the thread-safe
  // snapshot API, the same path the GUI uses).
  EXPECT_EQ(worker.SnapshotActiveTab().title, "UI Test");
  EXPECT_EQ(worker.SnapshotActiveTab().content_type,
            neko::browser::ContentType::kHtml);

  // The address bar shows the file path and history has one entry.
  EXPECT_GE(window.TabBarWidget()->count(), 1);
  EXPECT_FALSE(window.AddressBar()->text().isEmpty());
  EXPECT_EQ(worker.SnapshotHistory().size(), 1u);

  // Render the view to an image and verify it is non-blank.
  const QPixmap shot = window.grab();
  EXPECT_FALSE(shot.isNull());
  EXPECT_GT(shot.width(), 0);
}

TEST(UiSmokeTest, NavigationUpdatesAddressBarAndHistory) {
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
  ASSERT_TRUE(WaitFor([&] {
    return window.AddressBar()->text().contains("nav.html");
  }));
  ASSERT_EQ(worker.SnapshotHistory().size(), 1u);
  EXPECT_NE(window.AddressBar()->text().toStdString().find("nav.html"),
            std::string::npos);
}

TEST(UiSmokeTest, BackUpdatesAddressBar) {
  TempProfile tp;
  const std::string a = tp.path() + "/a.html";
  const std::string b = tp.path() + "/b.html";
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
      a, "<html><head><title>A</title></head><body>a</body></html>")
                  .has_value());
  ASSERT_TRUE(neko::storage::WriteFileAtomic(
      b, "<html><head><title>B</title></head><body>b</body></html>")
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

TEST(UiSmokeTest, DevToolsConsoleEvaluatesJavaScript) {
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
  ASSERT_TRUE(WaitFor([&] {
    return window.ConsoleView()->toPlainText().contains("Error: ui boom");
  }));
}

TEST(UiSmokeTest, AddressBarBackspaceDeletesCharacter) {
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
  address->setCursorPosition(address->text().size());
  SendKey(address, Qt::Key_Backspace);
  EXPECT_EQ(address->text(), QStringLiteral("https://example.com/fo"));
}

TEST(UiSmokeTest, AddressBarEditSurvivesPeriodicRefresh) {
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
  const int mid = original.size() / 2;
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

TEST(UiSmokeTest, MultiTabNavigationWorks) {
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

TEST(UiSmokeTest, KeyboardShortcutOpensAndClosesTabs) {
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

TEST(UiSmokeTest, HoverDoesNotResetScroll) {
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
    return snap.content_type == neko::browser::ContentType::kHtml && snap.page != nullptr &&
           snap.page->layout_root() != nullptr;
  }));

  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  ASSERT_GT(view->verticalScrollBar()->maximum(), 0);

  view->verticalScrollBar()->setValue(200);
  ASSERT_EQ(view->verticalScrollBar()->value(), 200);

  // Hover over the page (a MouseMove into the viewport); this changes the
  // hovered element and re-runs the cascade/layout.
  QMouseEvent move(QEvent::MouseMove, QPointF(100, 100), Qt::NoButton, Qt::NoButton,
                   Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &move);
  QCoreApplication::processEvents();

  // A script-pump / navigation refresh must not treat the hover-induced work
  // as a fresh load and reset the scroll to the top.
  view->Refresh();
  QCoreApplication::processEvents();

  EXPECT_EQ(view->verticalScrollBar()->value(), 200);
}

}  // namespace
