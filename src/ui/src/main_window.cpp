#include "neko/ui/main_window.h"

#include "neko/base/memory.h"
#include "neko/browser/preferences_keys.h"
#include "neko/browser/search_engines.h"
#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/style/computed_style.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/i18n.h"
#include "neko/ui/web_view.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace neko::ui {
namespace {

QString FromUtf8(std::string_view s)
{
  return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

} // namespace

MainWindow::~MainWindow()
{
  // The application outlives the window; stop listening to its focus changes
  // so a queued focusChanged can never touch members of a destroyed window.
  disconnect(qApp, &QApplication::focusChanged, this, nullptr);
}

MainWindow::MainWindow(BrowserWorker* worker, QWidget* parent)
    : QMainWindow(parent), worker_(worker)
{
  BuildUi();
  // Ensure at least one tab exists (created through the worker thread).
  if (worker_->SnapshotTabs().empty()) {
    worker_->NewTab("", true);
  }
  connect(worker_,
          &BrowserWorker::StateChanged,
          this,
          &MainWindow::OnStateChanged,
          Qt::QueuedConnection);
  connect(
      worker_,
      &BrowserWorker::DownloadFinished,
      this,
      [this](int, bool ok) {
        statusBar()->showMessage(
            ok ? QStringLiteral("Download finished.") : QStringLiteral("Download failed."), 4000);
        RefreshLists();
      },
      Qt::QueuedConnection);
  connect(
      worker_,
      &BrowserWorker::JavaScriptResult,
      this,
      [this](const QString& script, const QString& output, bool error) {
        if (!script.isEmpty()) {
          js_console_view_->appendPlainText(QStringLiteral("\u203a %1").arg(script));
        }
        if (!output.isEmpty()) {
          js_console_view_->appendPlainText(error ? QStringLiteral("\u2717 %1").arg(output)
                                                  : QStringLiteral("\u2190 %1").arg(output));
        }
      },
      Qt::QueuedConnection);
  setWindowTitle("Neko Browser");
  resize(1100, 750);

  // Pump page-script timers (setTimeout/setInterval) every 50 ms; the worker
  // emits StateChanged after each pumped action so the UI refreshes when a
  // timer callback mutates the DOM.
  script_timer_ = new QTimer(this);
  script_timer_->setInterval(50);
  connect(script_timer_, &QTimer::timeout, this, [this] { worker_->PumpScriptTimers(); });
  script_timer_->start();

  // Editing the address bar: while it has focus the periodic refresh must
  // not clobber the text or reset the cursor (that made Backspace appear to
  // delete nothing).  The moment the user focuses the bar it counts as an
  // edit; leaving it (or committing with Enter) lets the URL sync resume.
  connect(qApp,
          &QApplication::focusChanged,
          this,
          [self = QPointer<MainWindow>(this)](QWidget* old, QWidget* now) {
            if (self == nullptr) {
              return;
            }
            if (now == self->address_) {
              self->address_editing_ = true;
            } else if (old == self->address_) {
              self->address_editing_ = false;
            }
          });

  // Keyboard shortcuts (tab management + navigation).
  auto* new_tab = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), this);
  connect(new_tab, &QShortcut::activated, this, &MainWindow::OnNewTab);
  auto* close_tab = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_W), this);
  connect(close_tab, &QShortcut::activated, this, &MainWindow::CloseCurrentTab);
  auto* focus_bar = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), this);
  connect(focus_bar, &QShortcut::activated, this, &MainWindow::FocusAddressBar);
  auto* back_key = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
  connect(back_key, &QShortcut::activated, this, [this] { worker_->Back(); });
  auto* forward_key = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), this);
  connect(forward_key, &QShortcut::activated, this, [this] { worker_->Forward(); });
  auto* reload_key = new QShortcut(QKeySequence(Qt::Key_F5), this);
  connect(reload_key, &QShortcut::activated, this, [this] { worker_->Reload(); });
  auto* reload_key2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_R), this);
  connect(reload_key2, &QShortcut::activated, this, [this] { worker_->Reload(); });
  // Page zoom: browsers accept Ctrl+Plus, Ctrl+Equal and Ctrl+Minus; Ctrl+0
  // returns to 100%.
  for (const Qt::Key key : {Qt::Key_Plus, Qt::Key_Equal}) {
    auto* zoom_in_key = new QShortcut(QKeySequence(Qt::CTRL | key), this);
    connect(zoom_in_key, &QShortcut::activated, this, [this] { worker_->ZoomIn(); });
  }
  auto* zoom_out_key = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Minus), this);
  connect(zoom_out_key, &QShortcut::activated, this, [this] { worker_->ZoomOut(); });
  auto* zoom_reset_key = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), this);
  connect(zoom_reset_key, &QShortcut::activated, this, [this] { worker_->ResetZoom(); });
  // Find-in-page: Ctrl+F opens the bar, Enter / Shift+Enter step the matches.
  auto* find_key = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
  connect(find_key, &QShortcut::activated, this, [this] { ShowFindBar(); });
  // Ctrl+1..9 jump to the corresponding tab.
  for (int i = 1; i <= 9; ++i) {
    auto* jump =
        new QShortcut(QKeySequence(static_cast<int>(Qt::CTRL) | (Qt::Key_1 + i - 1)), this);
    connect(jump, &QShortcut::activated, this, [this, i] {
      if (i - 1 < tab_bar_->count())
        tab_bar_->setCurrentIndex(i - 1);
    });
  }

  RefreshAll();
}

void MainWindow::BuildUi()
{
  BuildToolbar();
  BuildPasswordBar();
  BuildBookmarkBar();
  BuildDocks();

  // Tab bar with a trailing "+" button (Ctrl+T / double-click also work).
  auto* tab_row = new QWidget(this);
  auto* tab_layout = new QHBoxLayout(tab_row);
  tab_layout->setContentsMargins(0, 0, 0, 0);
  tab_layout->setSpacing(0);
  tab_bar_ = new QTabBar(tab_row);
  tab_bar_->setTabsClosable(true);
  tab_bar_->setExpanding(false);
  tab_layout->addWidget(tab_bar_, 1);
  auto* new_tab_button = new QToolButton(tab_row);
  new_tab_button->setText(QStringLiteral("+"));
  new_tab_button->setAutoRaise(true);
  new_tab_button->setToolTip(tr("New tab (Ctrl+T)"));
  new_tab_button->setFixedSize(26, 26);
  connect(new_tab_button, &QToolButton::clicked, this, [this] { OnNewTab(); });
  tab_layout->addWidget(new_tab_button);

  pages_ = new QStackedWidget(this);
  connect(tab_bar_, &QTabBar::currentChanged, this, &MainWindow::OnTabBarChanged);
  connect(tab_bar_, &QTabBar::tabCloseRequested, this, &MainWindow::OnTabCloseRequested);
  connect(tab_bar_, &QTabBar::tabBarDoubleClicked, this, [this](int) { OnNewTab(); });

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(tab_row);
  layout->addWidget(pages_, 1);
  setCentralWidget(central);
}

void MainWindow::BuildToolbar()
{
  auto* toolbar = addToolBar(tr("Navigation"));
  toolbar->setMovable(false);
  toolbar->setObjectName(QStringLiteral("navigationToolbar"));

  auto* back = toolbar->addAction(tr("◀"), this, [this] {
    address_editing_ = false;
    worker_->Back();
  });
  auto* forward = toolbar->addAction(tr("▶"), this, [this] {
    address_editing_ = false;
    worker_->Forward();
  });
  auto* reload = toolbar->addAction(tr("⟳"), this, [this] {
    address_editing_ = false;
    worker_->Reload();
  });
  back->setToolTip(tr("Back"));
  forward->setToolTip(tr("Forward"));
  reload->setToolTip(tr("Reload"));
  auto* home = toolbar->addAction(tr("⌂"), this, [this] {
    address_editing_ = false;
    const QString home_page = Preference(FromUtf8(browser::prefs::kHomePage));
    if (!home_page.isEmpty()) {
      Navigate(home_page);
    }
  });
  home->setToolTip(tr("Home (set the home page in Settings)"));
  home_action_ = home;
  toolbar->addSeparator();

  address_ = new QLineEdit(this);
  address_->setPlaceholderText(tr("Enter URL or search..."));
  address_->setClearButtonEnabled(true);
  connect(address_, &QLineEdit::returnPressed, this, &MainWindow::OnNavigateRequested);
  connect(address_, &QLineEdit::textEdited, this, &MainWindow::OnAddressEdited);
  connect(address_, &QLineEdit::selectionChanged, this, &MainWindow::OnAddressEdited);
  toolbar->addWidget(address_);

  toolbar->addSeparator();
  auto* bookmark = toolbar->addAction(tr("★ Bookmark"), this, [this] { OnBookmark(); });
  bookmark->setToolTip(tr("Bookmark the current page"));

  auto* download = toolbar->addAction(tr("↓ Download"), this, [this] { OnDownloadActive(); });
  download->setToolTip(tr("Download the current URL"));

  toolbar->addSeparator();
  auto* zoom_out = toolbar->addAction(tr("−"), this, [this] { worker_->ZoomOut(); });
  zoom_out->setToolTip(tr("Zoom out (Ctrl+-)"));
  zoom_button_ = new QToolButton(this);
  zoom_button_->setText(tr("100%"));
  zoom_button_->setToolTip(tr("Page zoom — click to reset to 100% (Ctrl+0)"));
  zoom_button_->setAutoRaise(true);
  connect(zoom_button_, &QToolButton::clicked, this, [this] { worker_->ResetZoom(); });
  toolbar->addWidget(zoom_button_);
  auto* zoom_in = toolbar->addAction(tr("+"), this, [this] { worker_->ZoomIn(); });
  zoom_in->setToolTip(tr("Zoom in (Ctrl+=)"));

  BuildFindBar(toolbar);
}

QString MainWindow::Preference(const QString& key, const QString& fallback) const
{
  const auto preferences = worker_->SnapshotPreferences();
  const std::string wanted = key.toStdString();
  for (const auto& [entry_key, entry_value] : preferences) {
    if (entry_key == wanted) {
      return FromUtf8(entry_value);
    }
  }
  return fallback;
}

void MainWindow::SetPreference(const QString& key, const QString& value)
{
  worker_->SetPreference(key, value);
  // Apply the settings that affect the chrome immediately (the worker
  // confirms through the next StateChanged, which also re-syncs the widgets).
  if (key == FromUtf8(browser::prefs::kShowBookmarkBar) && bookmark_bar_ != nullptr) {
    bookmark_bar_->setVisible(value != QLatin1String("0"));
  }
}

void MainWindow::BuildPasswordBar()
{
  password_bar_ = new QToolBar(tr("Save password"), this);
  password_bar_->setMovable(false);
  password_bar_label_ = new QLabel(password_bar_);
  password_bar_->addWidget(password_bar_label_);
  password_save_action_ = password_bar_->addAction(tr("Save"));
  connect(password_save_action_, &QAction::triggered, this, &MainWindow::OnPasswordSaveClicked);
  password_dismiss_action_ = password_bar_->addAction(tr("Not now"));
  connect(
      password_dismiss_action_, &QAction::triggered, this, &MainWindow::OnPasswordDismissClicked);
  // A dedicated toolbar row directly under the navigation toolbar; hidden
  // until the active page submits a login form.
  addToolBarBreak(Qt::TopToolBarArea);
  addToolBar(Qt::TopToolBarArea, password_bar_);
  password_bar_->setVisible(false);
}

void MainWindow::BuildBookmarkBar()
{
  bookmark_bar_ = new QToolBar(tr("Bookmarks"), this);
  bookmark_bar_->setMovable(false);
  bookmark_bar_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(bookmark_bar_, &QToolBar::customContextMenuRequested, this, [this](const QPoint& pos) {
    QAction* action = bookmark_bar_->actionAt(pos);
    if (action == nullptr || action->data().toString().isEmpty()) {
      return;
    }
    const QString url = action->data().toString();
    QMenu menu(bookmark_bar_);
    QAction* open_new = menu.addAction(tr("Open in New Tab"));
    QAction* remove = menu.addAction(tr("Remove Bookmark"));
    const QAction* chosen = menu.exec(bookmark_bar_->mapToGlobal(pos));
    if (chosen == open_new) {
      worker_->NewTab(url, /*activate=*/false);
    } else if (chosen == remove) {
      worker_->RemoveBookmark(url);
    }
  });
  // The bookmark bar is a second toolbar row under the navigation toolbar;
  // visibility follows the show_bookmark_bar preference (default on).
  addToolBarBreak(Qt::TopToolBarArea);
  addToolBar(Qt::TopToolBarArea, bookmark_bar_);
  bookmark_bar_->setVisible(Preference(FromUtf8(browser::prefs::kShowBookmarkBar), "1") !=
                            QLatin1String("0"));
}

void MainWindow::SyncBookmarkBar()
{
  if (bookmark_bar_ == nullptr) {
    return;
  }
  const auto bookmarks = worker_->SnapshotBookmarks();
  // RefreshLists runs on every StateChanged (including hover/scroll), so only
  // rebuild the buttons when the bookmark set actually changed.
  QStringList signature;
  signature.reserve(static_cast<int>(bookmarks.size()));
  for (const auto& b : bookmarks) {
    signature.push_back(FromUtf8(b.url));
    signature.push_back(FromUtf8(b.title));
    signature.push_back(FromUtf8(b.folder));
  }
  if (signature == bookmark_bar_signature_) {
    return;
  }
  bookmark_bar_signature_ = signature;
  bookmark_bar_->clear();
  for (const auto& b : bookmarks) {
    const QString url = FromUtf8(b.url);
    const QString title = FromUtf8(b.title.empty() ? b.url : b.title);
    QAction* action = bookmark_bar_->addAction(
        title.size() > 30 ? title.left(30) + QStringLiteral("\u2026") : title, this, [this, url] {
          Navigate(url);
        });
    action->setData(url);
    action->setToolTip(url);
  }
}

void MainWindow::BuildFindBar(QToolBar* toolbar)
{
  // Hidden until Ctrl+F: a query input, a "current/total" counter and the
  // next/previous/close buttons.  The bar talks to the worker, which runs the
  // search on the active tab and reports it back through the tab snapshot.
  find_bar_ = new QWidget(this);
  auto* layout = new QHBoxLayout(find_bar_);
  layout->setContentsMargins(4, 0, 4, 0);
  layout->setSpacing(4);

  find_input_ = new QLineEdit(find_bar_);
  find_input_->setPlaceholderText(tr("Find in page"));
  find_input_->setClearButtonEnabled(true);
  find_input_->setMinimumWidth(220);
  layout->addWidget(find_input_);

  find_status_ = new QLabel(tr("0/0"), find_bar_);
  layout->addWidget(find_status_);

  auto* previous = new QToolButton(find_bar_);
  previous->setText(QString::fromUtf8("\u25b2"));
  previous->setToolTip(tr("Previous match (Shift+Enter)"));
  connect(
      previous, &QToolButton::clicked, this, [this] { worker_->Find(find_input_->text(), -1); });
  layout->addWidget(previous);

  auto* next = new QToolButton(find_bar_);
  next->setText(QString::fromUtf8("\u25bc"));
  next->setToolTip(tr("Next match (Enter)"));
  connect(next, &QToolButton::clicked, this, [this] { worker_->Find(find_input_->text(), 1); });
  layout->addWidget(next);

  auto* close = new QToolButton(find_bar_);
  close->setText(QString::fromUtf8("\u2715"));
  close->setToolTip(tr("Close find bar (Esc)"));
  connect(close, &QToolButton::clicked, this, [this] { HideFindBar(); });
  layout->addWidget(close);

  connect(find_input_, &QLineEdit::textChanged, this, [this](const QString& text) {
    if (syncing_find_)
      return;
    worker_->Find(text, 0);
  });
  // Enter steps forward, Shift+Enter backwards, Esc closes (see eventFilter:
  // a QShortcut would need a real focus chain, which synthetic key events in
  // tests and offscreen platforms do not provide).
  find_input_->installEventFilter(this);

  find_bar_->setVisible(false);
  if (toolbar != nullptr) {
    // The toolbar re-syncs a widget's visibility from its action, so the
    // action is what ShowFindBar/HideFindBar toggle.
    find_action_ = toolbar->addWidget(find_bar_);
    find_action_->setVisible(false);
  }
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == find_input_ && event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Escape) {
      HideFindBar();
      return true;
    }
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
      const int direction = key->modifiers().testFlag(Qt::ShiftModifier) ? -1 : 1;
      worker_->Find(find_input_->text(), direction);
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::ShowFindBar()
{
  if (find_bar_ == nullptr) {
    return;
  }
  if (find_action_ != nullptr) {
    find_action_->setVisible(true);
  }
  find_bar_->setVisible(true);
  find_input_->setFocus();
  find_input_->selectAll();
}

void MainWindow::HideFindBar()
{
  if (find_bar_ == nullptr) {
    return;
  }
  worker_->ClearFind();
  if (find_action_ != nullptr) {
    // The action owns the widget's visibility inside the toolbar (a widget-level
    // setVisible(false) alone leaves Qt's visibility flags inconsistent).
    find_action_->setVisible(false);
  }
  find_bar_->setVisible(false);
  syncing_find_ = true;
  find_input_->clear();
  syncing_find_ = false;
  find_status_->setText(tr("0/0"));
  if (WebView* view = ActiveView(); view != nullptr) {
    view->setFocus();
  }
}

void MainWindow::BuildDocks()
{
  // --- DevTools ---
  auto* devtools = new QTabWidget(this);
  dom_tree_ = new QTreeWidget(devtools);
  dom_tree_->setHeaderLabels({tr("Node"), tr("Text/Attributes")});
  connect(dom_tree_, &QTreeWidget::currentItemChanged, this, &MainWindow::OnDomSelectionChanged);
  // Computed style panel: shows the selected element's computed style.
  style_tree_ = new QTreeWidget(devtools);
  style_tree_->setHeaderLabels({tr("Property"), tr("Value")});
  style_tree_->setRootIsDecorated(false);
  style_tree_->setColumnCount(2);

  // Network tab: request log + a clear button.
  auto* network_widget = new QWidget(devtools);
  auto* network_layout = new QVBoxLayout(network_widget);
  network_layout->setContentsMargins(0, 0, 0, 0);
  network_layout->setSpacing(0);
  network_list_ = new QListWidget(network_widget);
  auto* clear_network = new QPushButton(tr("Clear log"), network_widget);
  clear_network->setMaximumHeight(26);
  connect(clear_network, &QPushButton::clicked, this, [this] { worker_->ClearNetworkLog(); });
  network_layout->addWidget(network_list_, 1);
  network_layout->addWidget(clear_network);

  // Cookies tab: cookies for the active page's origin.
  cookie_list_ = new QListWidget(devtools);

  // Console tab: engine console log on top, then a JS REPL (read-only
  // output + input line).  The REPL output is intentionally NOT touched by
  // RefreshDevTools(), which only repopulates the engine log.
  auto* console_widget = new QWidget(devtools);
  auto* console_layout = new QVBoxLayout(console_widget);
  console_layout->setContentsMargins(0, 0, 0, 0);
  console_layout->setSpacing(0);
  console_view_ = new QPlainTextEdit(console_widget);
  console_view_->setReadOnly(true);
  js_console_view_ = new QPlainTextEdit(console_widget);
  js_console_view_->setReadOnly(true);
  js_console_view_->setMaximumHeight(160);
  console_input_ = new QLineEdit(console_widget);
  console_input_->setPlaceholderText(tr("Evaluate JavaScript (Enter to run)"));
  connect(console_input_, &QLineEdit::returnPressed, this, &MainWindow::OnConsoleCommand);
  console_layout->addWidget(console_view_, 1);
  console_layout->addWidget(js_console_view_);
  console_layout->addWidget(console_input_);

  devtools->addTab(dom_tree_, tr("DOM"));
  devtools->addTab(style_tree_, tr("Computed"));
  devtools->addTab(network_widget, tr("Network"));
  devtools->addTab(cookie_list_, tr("Cookies"));
  devtools->addTab(console_widget, tr("Console"));

  auto* devtools_dock = new QDockWidget(tr("DevTools"), this);
  devtools_dock->setWidget(devtools);
  addDockWidget(Qt::RightDockWidgetArea, devtools_dock);

  // --- History --- (search box + delete/clear)
  auto* history_widget = new QWidget(this);
  auto* history_layout = new QVBoxLayout(history_widget);
  history_layout->setContentsMargins(0, 0, 0, 0);
  history_layout->setSpacing(0);
  history_search_ = new QLineEdit(history_widget);
  history_search_->setPlaceholderText(tr("Search history"));
  history_search_->setClearButtonEnabled(true);
  connect(history_search_, &QLineEdit::textChanged, this, [this] { RefreshLists(); });
  history_list_ = new QListWidget(history_widget);
  connect(history_list_, &QListWidget::itemActivated, this, &MainWindow::OnHistoryActivated);
  auto* history_buttons = new QWidget(history_widget);
  auto* history_button_layout = new QHBoxLayout(history_buttons);
  history_button_layout->setContentsMargins(0, 0, 0, 0);
  auto* history_delete = new QPushButton(tr("Delete"), history_buttons);
  connect(history_delete, &QPushButton::clicked, this, &MainWindow::OnHistoryDeleteSelected);
  auto* history_clear = new QPushButton(tr("Clear all"), history_buttons);
  connect(history_clear, &QPushButton::clicked, this, &MainWindow::OnHistoryClearAll);
  history_button_layout->addWidget(history_delete);
  history_button_layout->addWidget(history_clear);
  history_layout->addWidget(history_search_);
  history_layout->addWidget(history_list_, 1);
  history_layout->addWidget(history_buttons);
  auto* history_dock = new QDockWidget(tr("History"), this);
  history_dock->setWidget(history_widget);
  addDockWidget(Qt::RightDockWidgetArea, history_dock);

  // --- Bookmarks ---
  bookmark_list_ = new QListWidget(this);
  bookmark_list_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(bookmark_list_, &QListWidget::itemActivated, this, &MainWindow::OnBookmarkActivated);
  connect(
      bookmark_list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = bookmark_list_->itemAt(pos);
        if (item == nullptr)
          return;
        QMenu menu(this);
        QAction* remove = menu.addAction(tr("Remove bookmark"));
        if (menu.exec(bookmark_list_->mapToGlobal(pos)) == remove) {
          const QString url = item->data(Qt::UserRole).toString();
          worker_->RemoveBookmark(url);
        }
      });
  auto* bookmark_dock = new QDockWidget(tr("Bookmarks"), this);
  bookmark_dock->setWidget(bookmark_list_);
  addDockWidget(Qt::RightDockWidgetArea, bookmark_dock);

  // --- Downloads --- (double-click opens; open-folder / clear buttons)
  auto* download_widget = new QWidget(this);
  auto* download_layout = new QVBoxLayout(download_widget);
  download_layout->setContentsMargins(0, 0, 0, 0);
  download_layout->setSpacing(0);
  download_list_ = new QListWidget(download_widget);
  connect(download_list_, &QListWidget::itemDoubleClicked, this, &MainWindow::OnDownloadActivated);
  auto* download_buttons = new QWidget(download_widget);
  auto* download_button_layout = new QHBoxLayout(download_buttons);
  download_button_layout->setContentsMargins(0, 0, 0, 0);
  auto* download_folder = new QPushButton(tr("Open folder"), download_buttons);
  connect(download_folder, &QPushButton::clicked, this, &MainWindow::OnDownloadOpenFolder);
  auto* download_clear = new QPushButton(tr("Clear finished"), download_buttons);
  connect(download_clear, &QPushButton::clicked, this, &MainWindow::OnDownloadClearFinished);
  download_button_layout->addWidget(download_folder);
  download_button_layout->addWidget(download_clear);
  download_layout->addWidget(download_list_, 1);
  download_layout->addWidget(download_buttons);
  auto* download_dock = new QDockWidget(tr("Downloads"), this);
  download_dock->setWidget(download_widget);
  addDockWidget(Qt::RightDockWidgetArea, download_dock);

  // --- Settings ---
  auto* settings = new QWidget(this);
  auto* settings_layout = new QVBoxLayout(settings);

  settings_layout->addWidget(new QLabel(tr("Home page"), settings));
  home_page_edit_ = new QLineEdit(settings);
  home_page_edit_->setPlaceholderText(tr("https://... (opened by the Home button)"));
  connect(home_page_edit_, &QLineEdit::editingFinished, this, &MainWindow::OnHomePageEdited);
  settings_layout->addWidget(home_page_edit_);

  settings_layout->addWidget(new QLabel(tr("Default search engine"), settings));
  search_engine_combo_ = new QComboBox(settings);
  for (const browser::SearchEngine& engine : browser::BuiltinSearchEngines()) {
    search_engine_combo_->addItem(FromUtf8(engine.name), FromUtf8(engine.id));
  }
  connect(search_engine_combo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged),
          this,
          &MainWindow::OnSearchEngineChanged);
  settings_layout->addWidget(search_engine_combo_);

  settings_layout->addWidget(new QLabel(tr("Language"), settings));
  language_combo_ = new QComboBox(settings);
  // Empty data = follow the system locale (the default).
  language_combo_->addItem(tr("System default"), QString());
  for (const i18n::Language& language : i18n::SupportedLanguages()) {
    language_combo_->addItem(FromUtf8(language.native_name), FromUtf8(language.id));
  }
  connect(language_combo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged),
          this,
          &MainWindow::OnLanguageChanged);
  settings_layout->addWidget(language_combo_);

  bookmark_bar_check_ = new QCheckBox(tr("Show bookmark bar"), settings);
  connect(bookmark_bar_check_, &QCheckBox::toggled, this, &MainWindow::OnBookmarkBarToggled);
  settings_layout->addWidget(bookmark_bar_check_);

  settings_layout->addWidget(new QLabel(tr("Saved logins"), settings));
  password_list_ = new QListWidget(settings);
  password_list_->setMaximumHeight(110);
  settings_layout->addWidget(password_list_);
  auto* password_delete = new QPushButton(tr("Delete selected login"), settings);
  password_delete->setObjectName(QStringLiteral("passwordDeleteButton"));
  connect(password_delete, &QPushButton::clicked, this, &MainWindow::OnSavedLoginDelete);
  settings_layout->addWidget(password_delete);

  settings_profile_ = new QLabel(settings);
  settings_counts_ = new QLabel(settings);
  auto* clear = new QPushButton(tr("Clear cookies, history and bookmarks"), settings);
  connect(clear, &QPushButton::clicked, this, [this] {
    if (QMessageBox::question(
            this, tr("Clear data"), tr("Remove all cookies, history and bookmarks?")) ==
        QMessageBox::Yes) {
      worker_->ClearStorage();
    }
  });
  settings_layout->addWidget(settings_profile_);
  settings_layout->addWidget(settings_counts_);
  settings_layout->addWidget(clear);
  settings_layout->addStretch(1);
  auto* settings_dock = new QDockWidget(tr("Settings"), this);
  settings_dock->setWidget(settings);
  addDockWidget(Qt::RightDockWidgetArea, settings_dock);
  tabifyDockWidget(devtools_dock, history_dock);
  tabifyDockWidget(history_dock, bookmark_dock);
  tabifyDockWidget(bookmark_dock, download_dock);
  tabifyDockWidget(download_dock, settings_dock);
  devtools_dock->raise();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MainWindow::OnStateChanged()
{
  RefreshAll();
}

void MainWindow::OnAddressEdited()
{
  // The user typed or drag-selected in the address bar; stop refreshing its
  // text until the edit is committed.  Programmatic writes (RefreshAll) are
  // guarded by syncing_address_.
  if (!syncing_address_) {
    address_editing_ = true;
  }
}

void MainWindow::OnNavigateRequested()
{
  address_editing_ = false; // committed: let RefreshAll sync the URL again
  Navigate(address_->text());
}

void MainWindow::Navigate(const QString& input)
{
  if (input.trimmed().isEmpty())
    return;
  const int index = tab_bar_->currentIndex();
  if (index < 0) {
    worker_->NewTab(input, true);
    return;
  }
  const auto tabs = worker_->SnapshotTabs();
  if (index >= static_cast<int>(tabs.size()))
    return;
  worker_->Navigate(tabs[static_cast<size_t>(index)].id, input);
}

void MainWindow::OnTabBarChanged(int index)
{
  if (index < 0)
    return;
  // Switching tabs abandons any in-progress address-bar edit.
  address_editing_ = false;
  pages_->setCurrentIndex(index);
  const auto tabs = worker_->SnapshotTabs();
  if (index >= static_cast<int>(tabs.size()))
    return;
  worker_->ActivateTab(tabs[static_cast<size_t>(index)].id);
}

void MainWindow::OnTabCloseRequested(int index)
{
  const auto tabs = worker_->SnapshotTabs();
  if (index < 0 || index >= static_cast<int>(tabs.size()))
    return;
  worker_->CloseTab(tabs[static_cast<size_t>(index)].id);
  // The worker will emit StateChanged and re-sync the tab bar.
}

void MainWindow::OnNewTab()
{
  worker_->NewTab("", true);
}

void MainWindow::CloseCurrentTab()
{
  const int index = tab_bar_->currentIndex();
  const auto tabs = worker_->SnapshotTabs();
  if (index < 0 || index >= static_cast<int>(tabs.size()))
    return;
  // Closing the last tab leaves a fresh blank tab (browser convention).
  if (tabs.size() == 1) {
    worker_->CloseTab(tabs[static_cast<size_t>(index)].id);
    worker_->NewTab("", true);
    return;
  }
  worker_->CloseTab(tabs[static_cast<size_t>(index)].id);
}

void MainWindow::FocusAddressBar()
{
  address_->setFocus(Qt::ShortcutFocusReason);
  address_->selectAll();
}

void MainWindow::OnBookmark()
{
  worker_->BookmarkActive();
  statusBar()->showMessage(tr("Bookmarked."), 2000);
}

void MainWindow::OnDownloadActive()
{
  const browser::TabSnapshot tab = worker_->SnapshotActiveTab();
  if (tab.id < 0 || tab.url.empty())
    return;
  worker_->Download(FromUtf8(tab.url));
}

void MainWindow::OnHistoryActivated(QListWidgetItem* item)
{
  Navigate(item->data(Qt::UserRole).toString());
}

void MainWindow::OnBookmarkActivated(QListWidgetItem* item)
{
  Navigate(item->data(Qt::UserRole).toString());
}

void MainWindow::OnHistoryDeleteSelected()
{
  QListWidgetItem* item = history_list_->currentItem();
  if (item == nullptr) {
    return;
  }
  const QString url = item->data(Qt::UserRole).toString();
  if (!url.isEmpty()) {
    worker_->RemoveHistoryEntry(url);
  }
}

void MainWindow::OnHistoryClearAll()
{
  if (QMessageBox::question(this, tr("Clear history"), tr("Remove the entire browsing history?")) ==
      QMessageBox::Yes) {
    worker_->ClearHistory();
  }
}

void MainWindow::OnDownloadActivated(QListWidgetItem* item)
{
  if (item == nullptr) {
    return;
  }
  const QString path = item->data(Qt::UserRole).toString();
  if (path.isEmpty() || !QFileInfo::exists(path)) {
    statusBar()->showMessage(tr("The downloaded file is not available"), 3000);
    return;
  }
  QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::OnDownloadOpenFolder()
{
  const QString dir = worker_->DownloadDir();
  if (!dir.isEmpty()) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
  }
}

void MainWindow::OnDownloadClearFinished()
{
  worker_->ClearFinishedDownloads();
}

void MainWindow::OnSearchEngineChanged(int index)
{
  if (syncing_settings_ || index < 0) {
    return;
  }
  const QString id = search_engine_combo_->itemData(index).toString();
  if (!id.isEmpty()) {
    SetPreference(FromUtf8(browser::prefs::kSearchEngine), id);
  }
}

void MainWindow::OnHomePageEdited()
{
  if (syncing_settings_) {
    return;
  }
  SetPreference(FromUtf8(browser::prefs::kHomePage), home_page_edit_->text().trimmed());
}

void MainWindow::OnBookmarkBarToggled(bool visible)
{
  if (syncing_settings_) {
    return;
  }
  SetPreference(FromUtf8(browser::prefs::kShowBookmarkBar), visible ? "1" : "0");
}

void MainWindow::OnLanguageChanged(int index)
{
  if (syncing_settings_ || index < 0) {
    return;
  }
  // The translator is installed at startup, before any widget exists;
  // re-translating the live window (every label, tooltip and dock title) is a
  // restart-sized change, so the preference is applied on the next launch.
  SetPreference(FromUtf8(browser::prefs::kLanguage), language_combo_->itemData(index).toString());
  statusBar()->showMessage(tr("The language change takes effect after restarting the browser."),
                           5000);
}

void MainWindow::OnPasswordSaveClicked()
{
  const browser::TabSnapshot active = worker_->SnapshotActiveTab();
  if (active.id >= 0 && active.has_pending_credential) {
    worker_->SavePendingCredential(active.id);
  }
}

void MainWindow::OnPasswordDismissClicked()
{
  const browser::TabSnapshot active = worker_->SnapshotActiveTab();
  if (active.id >= 0 && active.has_pending_credential) {
    worker_->DismissPendingCredential(active.id);
  }
}

void MainWindow::OnSavedLoginDelete()
{
  if (password_list_ == nullptr) {
    return;
  }
  QListWidgetItem* item = password_list_->currentItem();
  if (item == nullptr) {
    return;
  }
  worker_->RemoveSavedLogin(item->data(Qt::UserRole).toString(),
                            item->data(Qt::UserRole + 1).toString());
}

void MainWindow::OnConsoleCommand()
{
  const QString script = console_input_->text().trimmed();
  if (script.isEmpty())
    return;
  console_input_->clear();
  js_console_view_->appendPlainText(QStringLiteral("\u203a %1").arg(script));
  worker_->EvaluateJavaScript(script);
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void MainWindow::RefreshAll()
{
  SyncTabs();
  // Address bar + title from the active tab.
  const browser::TabSnapshot tab = worker_->SnapshotActiveTab();
  if (tab.id >= 0) {
    // Don't clobber the address bar while the user is editing it (typing a
    // new URL or drag-selecting the text): a refresh mid-edit resets the
    // text and breaks the selection.  The programmatic write below is
    // guarded so it does not re-mark the bar as edited.
    if (!tab.url.empty() && !address_editing_) {
      syncing_address_ = true;
      address_->setText(FromUtf8(tab.url));
      syncing_address_ = false;
    }
    QString title = FromUtf8(tab.title);
    if (title.isEmpty())
      title = "Neko Browser";
    setWindowTitle(title + QStringLiteral(" — Neko Browser"));
  } else {
    address_->clear();
    setWindowTitle(QStringLiteral("Neko Browser"));
  }
  for (WebView* view : views_)
    view->Refresh();
  RefreshDevTools();
  RefreshLists();
}

void MainWindow::SyncTabs()
{
  const auto tabs = worker_->SnapshotTabs();
  const int active = worker_->ActiveTabIndex();
  const size_t tab_count = tabs.size();

  // Rebuild the per-tab views whenever the set/order of tab ids changes
  // (closing a tab moves the remaining Tabs inside the controller's vector,
  // so pointer identity is not stable — ids are).
  QVector<int> ids;
  ids.reserve(static_cast<int>(tab_count));
  for (const auto& tab : tabs)
    ids.push_back(tab.id);
  if (ids != view_ids_) {
    for (WebView* view : views_) {
      pages_->removeWidget(view);
      delete view;
    }
    views_.clear();
    view_ids_.clear();
    // The old views held the last GUI references to the closed tabs' Pages
    // and frames; hand their freed memory back to the OS (see
    // base::ReleaseFreeMemory -- glibc otherwise keeps it resident and the
    // RSS does not fall after closing a tab).
    neko::base::ReleaseFreeMemory();
    for (const auto& tab : tabs) {
      auto* view = new WebView(worker_, tab.id, this);
      views_.append(view);
      view_ids_.append(tab.id);
      pages_->addWidget(view);
    }
  }

  // Sync tab bar labels.
  for (int i = 0; i < tab_bar_->count() && i < static_cast<int>(tab_count); ++i) {
    const browser::TabSnapshot& tab = tabs[static_cast<size_t>(i)];
    QString label = FromUtf8(tab.title);
    if (label.isEmpty())
      label = FromUtf8(tab.url);
    if (label.isEmpty())
      label = tr("New Tab");
    if (label.size() > 24)
      label = label.left(24) + "\u2026";
    if (tab.loading) {
      label += " \u2026";
    }
    if (tab_bar_->tabText(i) != label)
      tab_bar_->setTabText(i, label);
  }
  while (tab_bar_->count() < static_cast<int>(tab_count)) {
    tab_bar_->addTab("");
  }
  while (tab_bar_->count() > static_cast<int>(tab_count)) {
    tab_bar_->removeTab(tab_bar_->count() - 1);
  }
  if (active >= 0 && active < tab_bar_->count() && tab_bar_->currentIndex() != active) {
    tab_bar_->setCurrentIndex(active);
  }
  if (active >= 0 && active < pages_->count() && pages_->currentIndex() != active) {
    pages_->setCurrentIndex(active);
  }
  if (zoom_button_ != nullptr && active >= 0 && active < static_cast<int>(tab_count)) {
    const int percent = static_cast<int>(tabs[static_cast<size_t>(active)].zoom * 100.0F + 0.5F);
    const QString text = QStringLiteral("%1%").arg(percent);
    if (zoom_button_->text() != text) {
      zoom_button_->setText(text);
    }
  }
  if (find_status_ != nullptr && active >= 0 && active < static_cast<int>(tab_count)) {
    const browser::TabSnapshot& tab = tabs[static_cast<size_t>(active)];
    const QString status =
        tab.find_match_count > 0 && tab.find_current_index >= 0
            ? QStringLiteral("%1/%2").arg(tab.find_current_index + 1).arg(tab.find_match_count)
            : QStringLiteral("0/%1").arg(tab.find_match_count);
    if (find_status_->text() != status) {
      find_status_->setText(status);
    }
  }
}

WebView* MainWindow::ActiveView() const
{
  const int index = pages_->currentIndex();
  if (index >= 0 && index < views_.size()) {
    return views_[index];
  }
  return nullptr;
}

void MainWindow::RefreshDevTools()
{
  PopulateDomTree(dom_tree_);
  PopulateCookies(cookie_list_);
  // If a node is selected in the DOM tree, keep its computed style panel in
  // sync. The selected element may have been removed by a page script even
  // when the page itself has not navigated.
  OnDomSelectionChanged();

  network_list_->clear();
  for (const auto& entry : worker_->SnapshotNetworkLog()) {
    QString line;
    if (entry.error.empty()) {
      line = QStringLiteral("%1 → %2 (%3 bytes)")
                 .arg(entry.status)
                 .arg(FromUtf8(entry.url))
                 .arg(static_cast<qulonglong>(entry.bytes));
    } else {
      line = QStringLiteral("ERROR %1 (%2)").arg(FromUtf8(entry.url)).arg(FromUtf8(entry.error));
    }
    network_list_->addItem(line);
  }

  console_view_->clear();
  for (const auto& entry : worker_->SnapshotConsoleLog()) {
    console_view_->appendPlainText(
        QStringLiteral("[%1] %2").arg(FromUtf8(entry.level), FromUtf8(entry.message)));
  }
}

void MainWindow::OnDomSelectionChanged()
{
  if (style_tree_ == nullptr)
    return;
  style_tree_->clear();
  QTreeWidgetItem* item = dom_tree_->currentItem();
  if (item == nullptr)
    return;
  const dom::Element* element =
      static_cast<const dom::Element*>(item->data(0, Qt::UserRole).value<void*>());
  if (element == nullptr)
    return;
  PopulateComputedStyle(style_tree_, item);
}

void MainWindow::PopulateCookies(QListWidget* list)
{
  list->clear();
  const auto cookies = worker_->SnapshotCookies();
  if (cookies.empty()) {
    list->addItem(tr("(no cookies)"));
    return;
  }
  for (const auto& cookie : cookies) {
    QString line = QStringLiteral("%1=%2  [%3]")
                       .arg(FromUtf8(cookie.name), FromUtf8(cookie.value), FromUtf8(cookie.domain));
    if (cookie.secure)
      line += tr("  Secure");
    if (cookie.http_only)
      line += tr("  HttpOnly");
    if (!cookie.same_site.empty()) {
      line += tr("  SameSite=%1").arg(FromUtf8(cookie.same_site));
    }
    list->addItem(line);
  }
}

void MainWindow::RefreshLists()
{
  const auto history = worker_->SnapshotHistory();
  const auto bookmarks = worker_->SnapshotBookmarks();
  const auto downloads = worker_->SnapshotDownloads();

  // History, filtered by the search box.
  const QString filter = history_search_ != nullptr ? history_search_->text().trimmed() : QString();
  history_list_->clear();
  for (const auto& entry : history) {
    const QString title = FromUtf8(entry.title.empty() ? entry.url : entry.title);
    const QString url = FromUtf8(entry.url);
    if (!filter.isEmpty() && !title.contains(filter, Qt::CaseInsensitive) &&
        !url.contains(filter, Qt::CaseInsensitive)) {
      continue;
    }
    auto* item = new QListWidgetItem(title, history_list_);
    item->setToolTip(url);
    item->setData(Qt::UserRole, url);
  }

  // Bookmarks.
  bookmark_list_->clear();
  for (const auto& b : bookmarks) {
    auto* item = new QListWidgetItem(
        FromUtf8(b.title.empty() ? b.url : b.title) +
            (b.folder.empty() ? QString() : QStringLiteral("  [%1]").arg(FromUtf8(b.folder))),
        bookmark_list_);
    item->setData(Qt::UserRole, FromUtf8(b.url));
  }

  // Downloads (double-click opens the file; the path rides in UserRole).
  download_list_->clear();
  for (const auto& d : downloads) {
    const QString total =
        d.total_bytes < 0 ? tr("?") : QString::number(static_cast<qulonglong>(d.total_bytes));
    auto* item =
        new QListWidgetItem(QStringLiteral("%1  %2  %3 / %4 bytes")
                                .arg(FromUtf8(browser::ToString(d.state)),
                                     FromUtf8(d.url),
                                     QString::number(static_cast<qulonglong>(d.received_bytes)),
                                     total),
                            download_list_);
    if (!d.filename.empty()) {
      item->setData(Qt::UserRole, FromUtf8(d.filename));
    }
    if (!d.error.empty()) {
      item->setToolTip(FromUtf8(d.error));
    }
  }

  // Settings (write the widgets programmatically; |syncing_settings_| keeps
  // the change signals from being mistaken for user edits; the home-page
  // editor is left alone while it has focus).
  if (settings_profile_ != nullptr) {
    settings_profile_->setText(tr("Profile: %1").arg(FromUtf8(worker_->profile_dir())));
    settings_counts_->setText(tr("Cookies: %1   History: %2   Bookmarks: %3   Downloads: %4")
                                  .arg(worker_->SnapshotCookieCount())
                                  .arg(history.size())
                                  .arg(bookmarks.size())
                                  .arg(downloads.size()));
  }
  syncing_settings_ = true;
  if (search_engine_combo_ != nullptr) {
    const QString engine =
        Preference(FromUtf8(browser::prefs::kSearchEngine), QStringLiteral("duckduckgo"));
    const int index = search_engine_combo_->findData(engine);
    if (index >= 0 && search_engine_combo_->currentIndex() != index) {
      search_engine_combo_->setCurrentIndex(index);
    }
  }
  if (language_combo_ != nullptr) {
    const QString language = Preference(FromUtf8(browser::prefs::kLanguage));
    const int index = language_combo_->findData(language);
    if (index >= 0 && language_combo_->currentIndex() != index) {
      language_combo_->setCurrentIndex(index);
    }
  }
  if (home_page_edit_ != nullptr && !home_page_edit_->hasFocus()) {
    const QString home = Preference(FromUtf8(browser::prefs::kHomePage));
    if (home_page_edit_->text() != home) {
      home_page_edit_->setText(home);
    }
  }
  if (bookmark_bar_check_ != nullptr) {
    bookmark_bar_check_->setChecked(Preference(FromUtf8(browser::prefs::kShowBookmarkBar), "1") !=
                                    QLatin1String("0"));
  }
  syncing_settings_ = false;

  // Password save prompt for the active tab (hidden unless a login form was
  // just submitted).
  if (password_bar_ != nullptr) {
    const browser::TabSnapshot active = worker_->SnapshotActiveTab();
    if (active.has_pending_credential) {
      password_bar_label_->setText(tr("Save password for %1 (%2)?")
                                       .arg(FromUtf8(active.pending_credential_origin),
                                            FromUtf8(active.pending_credential_username)));
      password_bar_->setVisible(true);
    } else {
      password_bar_->setVisible(false);
    }
  }

  // Saved logins (settings panel); rebuilt only when the set changed.
  if (password_list_ != nullptr) {
    const std::vector<browser::SavedLogin> logins = worker_->SavedLogins();
    QStringList signature;
    signature.reserve(static_cast<qsizetype>(logins.size()));
    for (const browser::SavedLogin& login : logins) {
      signature.push_back(FromUtf8(login.origin));
      signature.push_back(FromUtf8(login.username));
    }
    if (signature != password_list_signature_) {
      password_list_signature_ = signature;
      password_list_->clear();
      for (const browser::SavedLogin& login : logins) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 \u2014 %2").arg(FromUtf8(login.origin), FromUtf8(login.username)),
            password_list_);
        item->setData(Qt::UserRole, FromUtf8(login.origin));
        item->setData(Qt::UserRole + 1, FromUtf8(login.username));
      }
    }
  }

  SyncBookmarkBar();
}

void MainWindow::PopulateDomTree(QTreeWidget* tree)
{
  tree->clear();
  const browser::TabSnapshot tab = worker_->SnapshotActiveTab();
  if (tab.id < 0 || tab.content_type != browser::ContentType::kHtml || tab.page == nullptr) {
    return;
  }
  // ADR 0020: the DOM is owned by the worker thread, which mutates it while
  // running page scripts under Page's DOM lock.  Take the same lock so the
  // tree walk is serialized with those mutations (the snapshot alone does not
  // pin the document).
  std::unique_lock<std::recursive_mutex> dom_lock = tab.page->AcquireDomLock();
  const dom::Node* root = tab.page->document();
  if (root == nullptr)
    return;

  const std::function<void(const dom::Node*, QTreeWidgetItem*)> add = [&](const dom::Node* node,
                                                                          QTreeWidgetItem* parent) {
    if (node == nullptr)
      return;
    QString label;
    QString detail;
    if (node->node_type() == dom::NodeType::kElement) {
      const auto* element = static_cast<const dom::Element*>(node);
      label = QString("<%1>").arg(FromUtf8(element->tag_name()));
      for (const auto& attr : element->attributes()) {
        if (!detail.isEmpty())
          detail += ' ';
        detail += FromUtf8(attr.name) + "=\"" + FromUtf8(attr.value) + "\"";
      }
    } else if (node->node_type() == dom::NodeType::kText) {
      const auto* text = static_cast<const dom::Text*>(node);
      label = "#text";
      detail = FromUtf8(text->data());
      if (detail.size() > 60)
        detail = detail.left(60) + "\u2026";
    } else {
      label = QString::fromUtf8(node->node_name().data());
    }
    QTreeWidgetItem* item = new QTreeWidgetItem({label, detail});
    // Remember the element and the page it belongs to so the Computed panel
    // can show its style.  The snapshot held by the caller keeps the DOM
    // alive, but a later navigation replaces the page; store the page pointer
    // so PopulateComputedStyle can detect a stale element instead of
    // dereferencing freed memory.
    if (node->node_type() == dom::NodeType::kElement) {
      item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<const void*>(node)));
      item->setData(
          0, Qt::UserRole + 1, QVariant::fromValue(static_cast<const void*>(tab.page.get())));
    }
    if (parent == nullptr) {
      tree->addTopLevelItem(item);
    } else {
      parent->addChild(item);
    }
    for (dom::Node* child : node->ChildNodes())
      add(child, item);
  };
  add(root, nullptr);
  tree->expandAll();
}

void MainWindow::PopulateComputedStyle(QTreeWidget* tree, QTreeWidgetItem* item)
{
  const browser::TabSnapshot tab = worker_->SnapshotActiveTab();
  if (tab.id < 0 || tab.page == nullptr)
    return;
  // The element pointer was captured when the DOM tree was built; navigation
  // and same-page DOM mutation can both make it stale. The item stores the
  // page it came from, then Page validates the element under its mutex.
  const void* item_page = item->data(0, Qt::UserRole + 1).value<void*>();
  if (item_page != static_cast<const void*>(tab.page.get())) {
    return;
  }
  const auto* element =
      static_cast<const dom::Element*>(item->data(0, Qt::UserRole).value<void*>());
  if (element == nullptr)
    return;
  style::ComputedStyle style;
  std::string tag_name;
  if (!tab.page->TryGetComputedStyle(element, style, tag_name))
    return;

  const auto add_row = [&](const QString& prop, const QString& value) {
    auto* row = new QTreeWidgetItem(tree, {prop, value});
    row->setFirstColumnSpanned(false);
  };
  add_row(tr("tag"), FromUtf8(tag_name));
  add_row(tr("display"), FromUtf8(style::ToString(style.display)));
  add_row(tr("position"), FromUtf8(style::ToString(style.position)));
  const auto size_label = [](const std::optional<style::SizeSpec>& spec) {
    return spec.has_value() ? FromUtf8(style::ToString(spec.value())) : tr("auto");
  };
  add_row(tr("width"), size_label(style.width));
  add_row(tr("height"), size_label(style.height));
  add_row(tr("margin"),
          QStringLiteral("%1 %2 %3 %4")
              .arg(FromUtf8(style::ToString(style.margin_top)),
                   FromUtf8(style::ToString(style.margin_right)),
                   FromUtf8(style::ToString(style.margin_bottom)),
                   FromUtf8(style::ToString(style.margin_left))));
  add_row(tr("padding"),
          QStringLiteral("%1 %2 %3 %4")
              .arg(FromUtf8(style::ToString(style.padding_top)),
                   FromUtf8(style::ToString(style.padding_right)),
                   FromUtf8(style::ToString(style.padding_bottom)),
                   FromUtf8(style::ToString(style.padding_left))));
  add_row(tr("border"),
          QStringLiteral("%1 %2 %3 %4")
              .arg(FromUtf8(style::ToString(style.border_top)),
                   FromUtf8(style::ToString(style.border_right)),
                   FromUtf8(style::ToString(style.border_bottom)),
                   FromUtf8(style::ToString(style.border_left))));
  add_row(tr("font"),
          QStringLiteral("%1 %2 %3pt %4")
              .arg(FromUtf8(style.font_family))
              .arg(style.font_weight)
              .arg(static_cast<double>(style.font_size))
              .arg(style.font_italic ? tr("italic") : tr("normal")));
  add_row(tr("line-height"), QString::number(static_cast<double>(style.line_height)));
  add_row(tr("text-align"), FromUtf8(style::ToString(style.text_align)));
  add_row(tr("color"),
          style.color.has_value() ? FromUtf8(style::ToString(style.color.value()))
                                  : tr("(inherited)"));
  add_row(tr("background"),
          style.background_color.has_value()
              ? FromUtf8(style::ToString(style.background_color.value()))
              : tr("transparent"));
  add_row(tr("flex"),
          QStringLiteral("grow=%1 shrink=%2")
              .arg(static_cast<double>(style.flex_grow))
              .arg(static_cast<double>(style.flex_shrink)));
  if (style.flex_direction != style::FlexDirection::kRow) {
    add_row(tr("flex-direction"), FromUtf8(style::ToString(style.flex_direction)));
  }
  if (style.position == style::Position::kAbsolute || style.position == style::Position::kFixed) {
    add_row(tr("left"), FromUtf8(style::ToString(style.left)));
    add_row(tr("top"), FromUtf8(style::ToString(style.top)));
  }
  add_row(tr("order"), QString::number(style.order));
  if (!style.custom_properties.empty()) {
    for (const auto& [name, value] : style.custom_properties) {
      add_row(FromUtf8(name), FromUtf8(value));
    }
  }
  tree->expandAll();
}

} // namespace neko::ui
