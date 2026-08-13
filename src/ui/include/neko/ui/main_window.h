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
  qsizetype viewCount() const { return views_.size(); }

 private slots:
  void OnStateChanged();
  void OnNavigateRequested();
  void OnTabBarChanged(int index);
  void OnTabCloseRequested(int index);
  void OnNewTab();
  void OnBookmark();
  void OnDownloadActive();
  void OnHistoryActivated(QListWidgetItem* item);
  void OnBookmarkActivated(QListWidgetItem* item);

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
  QTabBar* tab_bar_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  QVector<WebView*> views_;
  QVector<int> view_ids_;  // tab id for each view, in order
  QLineEdit* address_ = nullptr;

  // Docks.
  QTreeWidget* dom_tree_ = nullptr;
  QListWidget* network_list_ = nullptr;
  QPlainTextEdit* console_view_ = nullptr;
  QListWidget* history_list_ = nullptr;
  QListWidget* bookmark_list_ = nullptr;
  QListWidget* download_list_ = nullptr;
  QLabel* settings_profile_ = nullptr;
  QLabel* settings_counts_ = nullptr;
};

}  // namespace neko::ui
