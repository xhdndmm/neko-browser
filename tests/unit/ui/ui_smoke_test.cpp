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
#include <QtTest>
#include <QWheelEvent>
#include <QTabBar>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "neko/browser/browser_controller.h"
#include "neko/dom/query.h"
#include "neko/layout/layout_tree.h"
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
  address->setCursorPosition(static_cast<int>(address->text().size()));
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
  QTest::mouseMove(view->viewport(), QPoint(static_cast<int>(100), static_cast<int>(100)));
  QCoreApplication::processEvents();

  // A script-pump / navigation refresh must not treat the hover-induced work
  // as a fresh load and reset the scroll to the top.
  view->Refresh();
  QCoreApplication::processEvents();

  EXPECT_EQ(view->verticalScrollBar()->value(), 200);
}

TEST(UiSmokeTest, ClickRunsPageClickListener) {
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
    return snap.page != nullptr && snap.page->layout_root() != nullptr;
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);

  // Click the button's center: body margin 0, button margin 10 + 200x40 →
  // the button spans roughly (10,10)..(210,50); (100,30) hits it.
  QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(static_cast<int>(100), static_cast<int>(30)));
  QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(static_cast<int>(100), static_cast<int>(30)));

  // The click is dispatched on the worker thread; wait until the page script
  // observed it and updated #status.
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* status = neko::dom::QuerySelector(*snap.page->document(), "#status");
    return status != nullptr && status->TextContent() == "yes";
  }));
}

// Finds the first laid-out text run belonging to |target| and returns its
// top-left point (document coordinates, before scroll).
bool FindElementRunPoint(const neko::layout::LayoutBox& box, const neko::dom::Element* target,
                         float& x, float& y) {
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

TEST(UiSmokeTest, ClickInputAndTypeUpdatesValue) {
  TempProfile tp;
  const std::string html =
      "<html><head><title>Input</title></head>"
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
    ASSERT_NE(snap.page->document(), nullptr);
    neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
    ASSERT_NE(input, nullptr);
    float x = 0;
    float y = 0;
    ASSERT_TRUE(FindElementRunPoint(*snap.page->layout_root(), input, x, y));
    QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(static_cast<int>(x), static_cast<int>(y)));
    QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(static_cast<int>(x), static_cast<int>(y)));
  }
  // The focused control is published on the page (read by caret painting).
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
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
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
    return input != nullptr && input->GetAttribute("value").value_or("") == "hia";
  }));
}

// Returns the caret point of |target|: the end of its first text run
// (document coordinates, before scroll).
bool FindCaretPoint(const neko::layout::LayoutBox& box, const neko::dom::Element* target,
                    float& x, float& y, float& h) {
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

TEST(UiSmokeTest, FocusedInputDrawsCaret) {
  TempProfile tp;
  const std::string html =
      "<html><head><title>I</title></head>"
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
    neko::dom::Element* input = neko::dom::QuerySelector(*snap.page->document(), "#q");
    float x = 0;
    float y = 0;
    float h = 0;
    ASSERT_TRUE(FindCaretPoint(*snap.page->layout_root(), input, x, y, h));
    QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(static_cast<int>(x - 2.0f), static_cast<int>(y)));
    QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                        QPoint(static_cast<int>(x - 2.0f), static_cast<int>(y)));
  }
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
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
    for (int y = std::max(0, static_cast<int>(cy)); y < static_cast<int>(cy + ch) && y < img.height(); ++y) {
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
    for (int y = std::max(0, static_cast<int>(cy2)); y < static_cast<int>(cy2 + ch2) && y < img.height(); ++y) {
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

TEST(UiSmokeTest, WheelFiresPageWheelEvent) {
  TempProfile tp;
  const std::string html =
      "<html><head><title>W</title></head>"
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
    return snap.page != nullptr && snap.page->layout_root() != nullptr;
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  view->Refresh();

  // One wheel notch: deltaY = the line step chosen by the view.
  QWheelEvent wheel(QPointF(100, 100), QPointF(100, 100), QPoint(0, 0), QPoint(0, -120),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, /*inverted=*/false);
  QApplication::sendEvent(view->viewport(), &wheel);

  // The page's onwheel handler observed a non-zero vertical delta.
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* body = neko::dom::QuerySelector(*snap.page->document(), "body");
    return body != nullptr && body->GetAttribute("data-d").has_value() &&
           std::stod(std::string(body->GetAttribute("data-d").value())) != 0;
  }));
}

TEST(UiSmokeTest, HoverOverElementFiresPageMouseOver) {
  TempProfile tp;
  const std::string html =
      "<html><head><title>H</title></head>"
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
    return snap.page != nullptr && snap.page->layout_root() != nullptr;
  }));
  auto* view = window.findChild<neko::ui::WebView*>();
  ASSERT_NE(view, nullptr);
  view->Refresh();

  // Move over the box (top-left of the page) -> page onmouseover.
  QTest::mouseMove(view->viewport(), QPoint(static_cast<int>(50), static_cast<int>(10)));
  ASSERT_TRUE(WaitFor([&] {
    const auto snap = worker.SnapshotActiveTab();
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
    if (snap.page == nullptr || snap.page->document() == nullptr) {
      return false;
    }
    neko::dom::Element* box = neko::dom::QuerySelector(*snap.page->document(), "#box");
    return box != nullptr && box->GetAttribute("data-h").value_or("") == "0";
  }));
}


TEST(UiSmokeTest, ClickLinkNavigates) {
  TempProfile tp;
  const std::string html =
      "<html><head><title>Link</title></head>"
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

  QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(static_cast<int>(x), static_cast<int>(y)));
  QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(static_cast<int>(x), static_cast<int>(y)));

  // The default action navigates the link to /nav.
  ASSERT_TRUE(WaitFor([&] {
    const auto s = worker.SnapshotActiveTab();
    return s.url.find("/nav") != std::string::npos;
  }));
}

TEST(UiSmokeTest, HoverLinkShowsPointingHand) {
  TempProfile tp;
  const std::string html =
      "<html><head><title>Hover</title></head>"
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

  // Hovering the hyperlink switches the pointer to a pointing hand.
  QTest::mouseMove(view->viewport(), QPoint(static_cast<int>(x), static_cast<int>(y)));
  EXPECT_EQ(view->viewport()->cursor().shape(), Qt::PointingHandCursor);

  // Hovering elsewhere restores the arrow.
  QTest::mouseMove(view->viewport(), QPoint(static_cast<int>(500), static_cast<int>(400)));
  EXPECT_EQ(view->viewport()->cursor().shape(), Qt::ArrowCursor);
}

}  // namespace
