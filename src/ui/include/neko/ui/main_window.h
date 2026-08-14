#pragma once

#include <QMainWindow>
#include <QVector>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QTabBar;
class QTreeWidget;
class QPlainTextEdit;
class QLabel;
class QTimer;

namespace neko::ui {

class BrowserWorker;
class WebView;

// The main browser window: toolbar + tabs + content view, plus DevTools,
// History, Bookmarks, Downloads and Settings docks.  It talks only to
// BrowserWorker (which owns the BrowserController).
class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(BrowserWorker* worker, QWidget* parent = nullptr);

  // Test/automation accessors.
  QTabBar* TabBarWidget() const { return tab_bar_; }
  QLineEdit* AddressBar() const { return address_; }
  QLineEdit* ConsoleInput() const { return console_input_; }
  QPlainTextEdit* ConsoleView() const { return js_console_view_; }
  qsizetype viewCount() const { return views_.size(); }

 private slots:
  void OnStateChanged();
  void OnNavigateRequested();
  void OnAddressEdited();
  void OnTabBarChanged(int index);
  void OnTabCloseRequested(int index);
  void OnNewTab();
  void OnBookmark();
  void OnDownloadActive();
  void OnHistoryActivated(QListWidgetItem* item);
  void OnBookmarkActivated(QListWidgetItem* item);
  void OnConsoleCommand();

 private:
  void BuildUi();
  void BuildToolbar();
  void BuildDocks();
  void RefreshAll();
  void SyncTabs();
  void RefreshDevTools();
  void RefreshLists();
  void Navigate(const QString& input);

  // DOM tree helpers.
  void PopulateDomTree(QTreeWidget* tree);

  BrowserWorker* worker_;
  // Periodically pumps the active page's script timers on the worker thread
  // so setTimeout/setInterval callbacks progress while the GUI idles.
  QTimer* script_timer_ = nullptr;
  QTabBar* tab_bar_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  QVector<WebView*> views_;
  QVector<int> view_ids_;  // tab id for each view, in order
  QLineEdit* address_ = nullptr;
  // True while the user is editing the address bar; RefreshAll() then leaves
  // the text alone instead of clobbering it with the tab's URL.
  bool address_editing_ = false;

  // Docks.
  QTreeWidget* dom_tree_ = nullptr;
  QListWidget* network_list_ = nullptr;
  QPlainTextEdit* console_view_ = nullptr;   // engine DevTools console log
  QPlainTextEdit* js_console_view_ = nullptr;  // JS REPL output (not cleared)
  QLineEdit* console_input_ = nullptr;
  QListWidget* history_list_ = nullptr;
  QListWidget* bookmark_list_ = nullptr;
  QListWidget* download_list_ = nullptr;
  QLabel* settings_profile_ = nullptr;
  QLabel* settings_counts_ = nullptr;
};

}  // namespace neko::ui
