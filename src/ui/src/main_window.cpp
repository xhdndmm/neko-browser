#include "neko/ui/main_window.h"

#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/style/computed_style.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/web_view.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
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
  connect(qApp, &QApplication::focusChanged, this, [this](QWidget* old, QWidget* now) {
    if (now == address_) {
      address_editing_ = true;
    } else if (old == address_) {
      address_editing_ = false;
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

  // --- History ---
  history_list_ = new QListWidget(this);
  connect(history_list_, &QListWidget::itemActivated, this, &MainWindow::OnHistoryActivated);
  auto* history_dock = new QDockWidget(tr("History"), this);
  history_dock->setWidget(history_list_);
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

  // --- Downloads ---
  download_list_ = new QListWidget(this);
  auto* download_dock = new QDockWidget(tr("Downloads"), this);
  download_dock->setWidget(download_list_);
  addDockWidget(Qt::RightDockWidgetArea, download_dock);

  // --- Settings ---
  auto* settings = new QWidget(this);
  auto* settings_layout = new QVBoxLayout(settings);
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
}

void MainWindow::RefreshDevTools()
{
  PopulateDomTree(dom_tree_);
  PopulateCookies(cookie_list_);
  // If a node is selected in the DOM tree, keep its computed style panel in
  // sync (the underlying element is identified by pointer, which stays valid
  // for the lifetime of the snapshot held by PopulateDomTree).
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

  // History.
  history_list_->clear();
  for (const auto& entry : history) {
    auto* item =
        new QListWidgetItem(FromUtf8(entry.title.empty() ? entry.url : entry.title), history_list_);
    item->setToolTip(FromUtf8(entry.url));
    item->setData(Qt::UserRole, FromUtf8(entry.url));
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

  // Downloads.
  download_list_->clear();
  for (const auto& d : downloads) {
    download_list_->addItem(QStringLiteral("%1  %2  %3 bytes")
                                .arg(FromUtf8(browser::ToString(d.state)))
                                .arg(FromUtf8(d.url))
                                .arg(static_cast<qulonglong>(d.received_bytes)));
  }

  // Settings.
  if (settings_profile_ != nullptr) {
    settings_profile_->setText(tr("Profile: %1").arg(FromUtf8(worker_->profile_dir())));
    settings_counts_->setText(tr("Cookies: %1   History: %2   Bookmarks: %3   Downloads: %4")
                                  .arg(worker_->SnapshotCookieCount())
                                  .arg(history.size())
                                  .arg(bookmarks.size())
                                  .arg(downloads.size()));
  }
}

void MainWindow::PopulateDomTree(QTreeWidget* tree)
{
  tree->clear();
  const browser::TabSnapshot tab = worker_->SnapshotActiveTab();
  if (tab.id < 0 || tab.content_type != browser::ContentType::kHtml || tab.page == nullptr) {
    return;
  }
  // The snapshot keeps the page (and its DOM) alive; a published page is
  // never mutated by the worker, so walking the document is safe here.
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
      item->setData(0, Qt::UserRole + 1,
                    QVariant::fromValue(static_cast<const void*>(tab.page.get())));
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
  // The element pointer was captured when the DOM tree was built; if the
  // page has since navigated, the pointer may dangle.  The item stores the
  // page it came from — only use the pointer when it still belongs to the
  // active page.
  const void* item_page = item->data(0, Qt::UserRole + 1).value<void*>();
  if (item_page != static_cast<const void*>(tab.page.get())) {
    return;
  }
  const auto* element =
      static_cast<const dom::Element*>(item->data(0, Qt::UserRole).value<void*>());
  if (element == nullptr)
    return;
  const style::ComputedStyle& style = tab.page->styles().StyleFor(*element);

  const auto add_row = [&](const QString& prop, const QString& value) {
    auto* row = new QTreeWidgetItem(tree, {prop, value});
    row->setFirstColumnSpanned(false);
  };
  add_row(tr("tag"), FromUtf8(element->tag_name()));
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
              .arg(style.font_size)
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
          QStringLiteral("grow=%1 shrink=%2").arg(style.flex_grow).arg(style.flex_shrink));
  if (style.flex_direction != style::FlexDirection::kRow) {
    add_row(tr("flex-direction"), FromUtf8(style::ToString(style.flex_direction)));
  }
  if (style.position == style::Position::kAbsolute || style.position == style::Position::kFixed) {
    add_row(tr("left"), QString::number(static_cast<double>(style.left)));
    add_row(tr("top"), QString::number(static_cast<double>(style.top)));
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
