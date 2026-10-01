#pragma once

#include <QMainWindow>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QTabBar;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;
class QLabel;
class QTimer;
class QToolButton;

namespace neko::ui {

class BrowserWorker;
class WebView;

// The main browser window: toolbar + tabs + content view, plus DevTools,
// History, Bookmarks, Downloads and Settings docks.  It talks only to
// BrowserWorker (which owns the BrowserController).
class MainWindow : public QMainWindow
{
  Q_OBJECT
public:
  explicit MainWindow(BrowserWorker* worker, QWidget* parent = nullptr);
  ~MainWindow() override;

  // Test/automation accessors.
  QTabBar* TabBarWidget() const
  {
    return tab_bar_;
  }
  QLineEdit* AddressBar() const
  {
    return address_;
  }
  // The find-in-page bar's input and visibility (Ctrl+F).
  QLineEdit* FindInput() const
  {
    return find_input_;
  }
  QWidget* FindBar() const
  {
    return find_bar_;
  }
  // True while the find bar is shown (the toolbar action owns its visibility).
  bool FindBarShowing() const
  {
    return find_action_ != nullptr && find_action_->isVisible();
  }
  QLabel* FindStatus() const
  {
    return find_status_;
  }
  // The toolbar's page-zoom indicator ("100%"); clicking it resets the zoom.
  QToolButton* ZoomIndicator() const
  {
    return zoom_button_;
  }
  QLineEdit* ConsoleInput() const
  {
    return console_input_;
  }
  QPlainTextEdit* ConsoleView() const
  {
    return js_console_view_;
  }
  qsizetype viewCount() const
  {
    return views_.size();
  }
  // The view for the active tab, or nullptr (for GUI tests that need direct
  // access to a WebView's scroll bar).
  WebView* ActiveView() const;
  // Feature accessors (tests / automation).
  QToolBar* BookmarkBarWidget() const
  {
    return bookmark_bar_;
  }
  QComboBox* SearchEngineComboWidget() const
  {
    return search_engine_combo_;
  }
  QLineEdit* HomePageEditWidget() const
  {
    return home_page_edit_;
  }
  QCheckBox* BookmarkBarCheckWidget() const
  {
    return bookmark_bar_check_;
  }
  QLineEdit* HistorySearchWidget() const
  {
    return history_search_;
  }
  QListWidget* HistoryListWidget() const
  {
    return history_list_;
  }
  QListWidget* DownloadListWidget() const
  {
    return download_list_;
  }
  QAction* HomeAction() const
  {
    return home_action_;
  }

private slots:
  void OnStateChanged();
  void OnNavigateRequested();
  void OnAddressEdited();
  void OnTabBarChanged(int index);
  void OnTabCloseRequested(int index);
  void OnNewTab();
  void CloseCurrentTab();
  void OnBookmark();
  void OnDownloadActive();
  void OnHistoryActivated(QListWidgetItem* item);
  void OnBookmarkActivated(QListWidgetItem* item);
  void OnConsoleCommand();
  void OnDomSelectionChanged();
  // History panel management.
  void OnHistoryDeleteSelected();
  void OnHistoryClearAll();
  // Downloads panel.
  void OnDownloadActivated(QListWidgetItem* item);
  void OnDownloadOpenFolder();
  void OnDownloadClearFinished();
  // Settings panel.
  void OnSearchEngineChanged(int index);
  void OnHomePageEdited();
  void OnBookmarkBarToggled(bool visible);

private:
  // Enter/Shift+Enter in the find input step the matches (handled here so the
  // shift state comes from the key event itself, not global keyboard state).
  bool eventFilter(QObject* watched, QEvent* event) override;
  void BuildUi();
  void BuildToolbar();
  void BuildBookmarkBar();
  void BuildFindBar(QToolBar* toolbar);
  void ShowFindBar();
  void HideFindBar();
  void BuildDocks();
  void RefreshAll();
  void SyncTabs();
  void SyncBookmarkBar();
  void RefreshDevTools();
  void RefreshLists();
  // Persists one preference and refreshes the tab-independent UI (settings,
  // bookmark bar) immediately.
  void SetPreference(const QString& key, const QString& value);
  QString Preference(const QString& key, const QString& fallback = QString()) const;
  void Navigate(const QString& input);
  void FocusAddressBar();

  // DOM tree helpers.
  void PopulateDomTree(QTreeWidget* tree);
  void PopulateComputedStyle(QTreeWidget* tree, QTreeWidgetItem* item);
  void PopulateCookies(QListWidget* list);

  BrowserWorker* worker_;
  // Periodically pumps the active page's script timers on the worker thread
  // so setTimeout/setInterval callbacks progress while the GUI idles.
  QTimer* script_timer_ = nullptr;
  QTabBar* tab_bar_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  QVector<WebView*> views_;
  QVector<int> view_ids_; // tab id for each view, in order
  QLineEdit* address_ = nullptr;
  // Page-zoom indicator in the toolbar ("100%"); clicking it resets to 100%.
  QToolButton* zoom_button_ = nullptr;
  // The toolbar's Home button (opens the home_page preference).
  QAction* home_action_ = nullptr;
  // Find-in-page bar (Ctrl+F): query input, "n/m" status and its container
  // (hidden until Ctrl+F).
  QWidget* find_bar_ = nullptr;
  QLineEdit* find_input_ = nullptr;
  QLabel* find_status_ = nullptr;
  // The action the find bar lives in: the toolbar syncs a widget's visibility
  // from its action, so hiding/showing goes through this.
  QAction* find_action_ = nullptr;
  // Set while HideFindBar clears the input, so the programmatic text change is
  // not mistaken for a user edit (which would re-run the query).
  bool syncing_find_ = false;
  // True while the user is editing the address bar; RefreshAll() then leaves
  // the text alone instead of clobbering it with the tab's URL.
  bool address_editing_ = false;
  // Set while RefreshAll() writes the URL programmatically, so the
  // selectionChanged signal it triggers is not mistaken for a user edit.
  bool syncing_address_ = false;

  // Docks.
  QTreeWidget* dom_tree_ = nullptr;
  QTreeWidget* style_tree_ = nullptr; // computed style of the selected node
  QListWidget* network_list_ = nullptr;
  QListWidget* cookie_list_ = nullptr;        // cookies of the active page
  QPlainTextEdit* console_view_ = nullptr;    // engine DevTools console log
  QPlainTextEdit* js_console_view_ = nullptr; // JS REPL output (not cleared)
  QLineEdit* console_input_ = nullptr;
  QListWidget* history_list_ = nullptr;
  QLineEdit* history_search_ = nullptr;
  QListWidget* bookmark_list_ = nullptr;
  // The bookmark bar (a toolbar row under the main toolbar) and the signature
  // of what it currently shows, so RefreshLists only rebuilds it on change.
  QToolBar* bookmark_bar_ = nullptr;
  QStringList bookmark_bar_signature_;
  QListWidget* download_list_ = nullptr;
  QLabel* settings_profile_ = nullptr;
  QLabel* settings_counts_ = nullptr;
  QComboBox* search_engine_combo_ = nullptr;
  QLineEdit* home_page_edit_ = nullptr;
  QCheckBox* bookmark_bar_check_ = nullptr;
  // True while RefreshLists writes the settings widgets programmatically, so
  // the signals they emit are not mistaken for user edits.
  bool syncing_settings_ = false;
};

} // namespace neko::ui
