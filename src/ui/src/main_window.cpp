#include "neko/ui/main_window.h"

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
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "neko/dom/element.h"
#include "neko/dom/node.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/web_view.h"

namespace neko::ui {
namespace {

QString FromUtf8(std::string_view s) {
  return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

}  // namespace

MainWindow::MainWindow(BrowserWorker* worker, QWidget* parent)
    : QMainWindow(parent), worker_(worker) {
  BuildUi();
  // Ensure at least one tab exists (created through the worker thread).
  if (worker_->controller().tabs().empty()) {
    worker_->NewTab("", true);
  }
  connect(worker_, &BrowserWorker::StateChanged, this, &MainWindow::OnStateChanged,
          Qt::QueuedConnection);
  connect(worker_, &BrowserWorker::DownloadFinished, this,
          [this](int, bool ok) {
            statusBar()->showMessage(ok ? QStringLiteral("Download finished.")
                                        : QStringLiteral("Download failed."),
                                     4000);
            RefreshLists();
          },
          Qt::QueuedConnection);
  connect(worker_, &BrowserWorker::JavaScriptResult, this,
          [this](const QString& script, const QString& output, bool error) {
            if (!script.isEmpty()) {
              js_console_view_->appendPlainText(QStringLiteral("\u203a %1").arg(script));
            }
            if (!output.isEmpty()) {
              js_console_view_->appendPlainText(
                  error ? QStringLiteral("\u2717 %1").arg(output)
                        : QStringLiteral("\u2190 %1").arg(output));
            }
          },
          Qt::QueuedConnection);
  setWindowTitle("Neko Browser");
  resize(1100, 750);
  RefreshAll();
}

void MainWindow::BuildUi() {
  BuildToolbar();
  BuildDocks();

  tab_bar_ = new QTabBar(this);
  tab_bar_->setTabsClosable(true);
  tab_bar_->setExpanding(false);
  pages_ = new QStackedWidget(this);
  connect(tab_bar_, &QTabBar::currentChanged, this, &MainWindow::OnTabBarChanged);
  connect(tab_bar_, &QTabBar::tabCloseRequested, this, &MainWindow::OnTabCloseRequested);
  connect(tab_bar_, &QTabBar::tabBarDoubleClicked, this, [this](int) { OnNewTab(); });

  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(tab_bar_);
  layout->addWidget(pages_, 1);
  setCentralWidget(central);
}

void MainWindow::BuildToolbar() {
  auto* toolbar = addToolBar(tr("Navigation"));
  toolbar->setMovable(false);

  auto* back = toolbar->addAction(tr("◀"), this, [this] { worker_->Back(); });
  auto* forward = toolbar->addAction(tr("▶"), this, [this] { worker_->Forward(); });
  auto* reload = toolbar->addAction(tr("⟳"), this, [this] { worker_->Reload(); });
  back->setToolTip(tr("Back"));
  forward->setToolTip(tr("Forward"));
  reload->setToolTip(tr("Reload"));
  toolbar->addSeparator();

  address_ = new QLineEdit(this);
  address_->setPlaceholderText(tr("Enter URL or search..."));
  address_->setClearButtonEnabled(true);
  connect(address_, &QLineEdit::returnPressed, this, &MainWindow::OnNavigateRequested);
  toolbar->addWidget(address_);

  toolbar->addSeparator();
  auto* bookmark = toolbar->addAction(tr("★ Bookmark"), this, [this] { OnBookmark(); });
  bookmark->setToolTip(tr("Bookmark the current page"));

  auto* download = toolbar->addAction(tr("↓ Download"), this, [this] { OnDownloadActive(); });
  download->setToolTip(tr("Download the current URL"));
}

void MainWindow::BuildDocks() {
  // --- DevTools ---
  auto* devtools = new QTabWidget(this);
  dom_tree_ = new QTreeWidget(devtools);
  dom_tree_->setHeaderLabels({ tr("Node"), tr("Text/Attributes") });
  network_list_ = new QListWidget(devtools);

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
  devtools->addTab(network_list_, tr("Network"));
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
  connect(bookmark_list_, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) {
            QListWidgetItem* item = bookmark_list_->itemAt(pos);
            if (item == nullptr) return;
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
    if (QMessageBox::question(this, tr("Clear data"),
                              tr("Remove all cookies, history and bookmarks?")) ==
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

void MainWindow::OnStateChanged() { RefreshAll(); }

void MainWindow::OnNavigateRequested() {
  Navigate(address_->text());
}

void MainWindow::Navigate(const QString& input) {
  if (input.trimmed().isEmpty()) return;
  const int index = tab_bar_->currentIndex();
  if (index < 0) {
    worker_->NewTab(input, true);
    return;
  }
  worker_->Navigate(worker_->controller().tabs()[static_cast<size_t>(index)]->id, input);
}

void MainWindow::OnTabBarChanged(int index) {
  if (index < 0) return;
  pages_->setCurrentIndex(index);
  worker_->ActivateTab(worker_->controller().tabs()[static_cast<size_t>(index)]->id);
}

void MainWindow::OnTabCloseRequested(int index) {
  const auto& tabs = worker_->controller().tabs();
  if (index >= static_cast<int>(tabs.size())) return;
  worker_->CloseTab(tabs[static_cast<size_t>(index)]->id);
  // The worker will emit StateChanged and re-sync the tab bar.
}

void MainWindow::OnNewTab() { worker_->NewTab("", true); }

void MainWindow::OnBookmark() {
  worker_->BookmarkActive();
  statusBar()->showMessage(tr("Bookmarked."), 2000);
}

void MainWindow::OnDownloadActive() {
  const auto* tab = worker_->controller().ActiveTab();
  if (tab == nullptr || tab->url.empty()) return;
  worker_->Download(FromUtf8(tab->url));
}

void MainWindow::OnHistoryActivated(QListWidgetItem* item) {
  Navigate(item->data(Qt::UserRole).toString());
}

void MainWindow::OnBookmarkActivated(QListWidgetItem* item) {
  Navigate(item->data(Qt::UserRole).toString());
}

void MainWindow::OnConsoleCommand() {
  const QString script = console_input_->text().trimmed();
  if (script.isEmpty()) return;
  console_input_->clear();
  js_console_view_->appendPlainText(QStringLiteral("\u203a %1").arg(script));
  worker_->EvaluateJavaScript(script);
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void MainWindow::RefreshAll() {
  SyncTabs();
  // Address bar + title from the active tab.
  const auto* tab = worker_->controller().ActiveTab();
  if (tab != nullptr) {
    if (!tab->url.empty()) {
      address_->setText(FromUtf8(tab->url));
    }
    QString title = FromUtf8(tab->title);
    if (title.isEmpty()) title = "Neko Browser";
    setWindowTitle(title + QStringLiteral(" — Neko Browser"));
  } else {
    address_->clear();
    setWindowTitle(QStringLiteral("Neko Browser"));
  }
  for (WebView* view : views_) view->Refresh();
  RefreshDevTools();
  RefreshLists();
}

void MainWindow::SyncTabs() {
  const auto& tabs = worker_->controller().tabs();
  const int active = worker_->controller().active_tab();
  const size_t tab_count = tabs.size();

  // Rebuild the per-tab views whenever the set/order of tab ids changes
  // (closing a tab moves the remaining Tabs inside the controller's vector,
  // so pointer identity is not stable — ids are).
  QVector<int> ids;
  ids.reserve(static_cast<int>(tab_count));
  for (const auto& tab : tabs) ids.push_back(tab->id);
  if (ids != view_ids_) {
    for (WebView* view : views_) {
      pages_->removeWidget(view);
      delete view;
    }
    views_.clear();
    view_ids_.clear();
    for (const auto& tab : tabs) {
      auto* view = new WebView(worker_, tab->id, this);
      views_.append(view);
      view_ids_.append(tab->id);
      pages_->addWidget(view);
    }
  }

  // Sync tab bar labels.
  for (int i = 0; i < tab_bar_->count() && i < static_cast<int>(tab_count); ++i) {
    const auto& tab = tabs[static_cast<size_t>(i)];
    QString label = FromUtf8(tab->title);
    if (label.isEmpty()) label = FromUtf8(tab->url);
    if (label.isEmpty()) label = tr("New Tab");
    if (label.size() > 24) label = label.left(24) + "\u2026";
    if (tab->loading) {
      label += " \u2026";
    }
    if (tab_bar_->tabText(i) != label) tab_bar_->setTabText(i, label);
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

void MainWindow::RefreshDevTools() {
  PopulateDomTree(dom_tree_);

  network_list_->clear();
  for (const auto& entry : worker_->controller().network_log()) {
    QString line;
    if (entry.error.empty()) {
      line = QStringLiteral("%1 → %2 (%3 bytes)").arg(entry.status).arg(FromUtf8(entry.url))
                 .arg(static_cast<qulonglong>(entry.bytes));
    } else {
      line = QStringLiteral("ERROR %1 (%2)").arg(FromUtf8(entry.url))
                 .arg(FromUtf8(entry.error));
    }
    network_list_->addItem(line);
  }

  console_view_->clear();
  for (const auto& entry : worker_->controller().console_log()) {
    console_view_->appendPlainText(
        QStringLiteral("[%1] %2").arg(FromUtf8(entry.level), FromUtf8(entry.message)));
  }
}

void MainWindow::RefreshLists() {
  // History.
  history_list_->clear();
  for (const auto& entry : worker_->controller().history().All()) {
    auto* item = new QListWidgetItem(FromUtf8(entry.title.empty() ? entry.url : entry.title),
                                     history_list_);
    item->setToolTip(FromUtf8(entry.url));
    item->setData(Qt::UserRole, FromUtf8(entry.url));
  }

  // Bookmarks.
  bookmark_list_->clear();
  for (const auto& b : worker_->controller().bookmarks().All()) {
    auto* item = new QListWidgetItem(
        FromUtf8(b.title.empty() ? b.url : b.title) +
            (b.folder.empty() ? QString() : QStringLiteral("  [%1]").arg(FromUtf8(b.folder))),
        bookmark_list_);
    item->setData(Qt::UserRole, FromUtf8(b.url));
  }

  // Downloads.
  download_list_->clear();
  for (const auto& d : worker_->controller().downloads().items()) {
    download_list_->addItem(QStringLiteral("%1  %2  %3 bytes")
                                .arg(FromUtf8(browser::ToString(d.state)))
                                .arg(FromUtf8(d.url))
                                .arg(static_cast<qulonglong>(d.received_bytes)));
  }

  // Settings.
  if (settings_profile_ != nullptr) {
    settings_profile_->setText(tr("Profile: %1")
                                   .arg(FromUtf8(worker_->controller().profile_dir())));
    settings_counts_->setText(
        tr("Cookies: %1   History: %2   Bookmarks: %3   Downloads: %4")
            .arg(worker_->controller().cookies().size())
            .arg(worker_->controller().history().size())
            .arg(worker_->controller().bookmarks().size())
            .arg(worker_->controller().downloads().size()));
  }
}

void MainWindow::PopulateDomTree(QTreeWidget* tree) {
  tree->clear();
  auto* tab = worker_->controller().ActiveTab();
  if (tab == nullptr || tab->content_type != browser::ContentType::kHtml) return;
  const dom::Node* root = tab->page.document();
  if (root == nullptr) return;

  const std::function<void(const dom::Node*, QTreeWidgetItem*)> add = [&](const dom::Node* node,
                                                                          QTreeWidgetItem* parent) {
    if (node == nullptr) return;
    QString label;
    QString detail;
    if (node->node_type() == dom::NodeType::kElement) {
      const auto* element = static_cast<const dom::Element*>(node);
      label = QString("<%1>").arg(FromUtf8(element->tag_name()));
      for (const auto& attr : element->attributes()) {
        if (!detail.isEmpty()) detail += ' ';
        detail += FromUtf8(attr.name) + "=\"" + FromUtf8(attr.value) + "\"";
      }
    } else if (node->node_type() == dom::NodeType::kText) {
      const auto* text = static_cast<const dom::Text*>(node);
      label = "#text";
      detail = FromUtf8(text->data());
      if (detail.size() > 60) detail = detail.left(60) + "\u2026";
    } else {
      label = QString::fromUtf8(node->node_name().data());
    }
    QTreeWidgetItem* item = new QTreeWidgetItem({ label, detail });
    if (parent == nullptr) {
      tree->addTopLevelItem(item);
    } else {
      parent->addChild(item);
    }
    for (dom::Node* child : node->ChildNodes()) add(child, item);
  };
  add(root, nullptr);
  tree->expandAll();
}

}  // namespace neko::ui
