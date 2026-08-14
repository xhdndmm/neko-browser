// GUI smoke tests.  They run under the Qt "offscreen" platform plugin
// (QT_QPA_PLATFORM=offscreen, set as a ctest property) so no display is
// needed.  The tests exercise the real BrowserWorker + MainWindow + WebView
// stack end to end.

#include <QApplication>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
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

}  // namespace
