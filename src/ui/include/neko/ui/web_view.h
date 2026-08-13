#pragma once

#include <QWidget>

#include "neko/browser/browser_controller.h"

class QPlainTextEdit;

namespace neko::ui {

class BrowserWorker;

// Paints the content of one tab: rendered HTML (through the engine
// pipeline), decoded images, PDF/audio/text summaries or error messages.
//
// The tab is identified by id and resolved through the controller each time
// so that closing a tab (which may move other tabs in the controller's
// vector) can never leave this view holding a stale pointer.
class WebView : public QWidget {
  Q_OBJECT
 public:
  WebView(BrowserWorker* worker, int tab_id, QWidget* parent = nullptr);

  void Refresh();  // re-layout/re-render and resync the text overlay
  int tab_id() const { return tab_id_; }

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  browser::Tab* CurrentTab() const;
  void PaintHtml(QPainter& painter, browser::Tab* tab);
  void PaintImage(QPainter& painter, browser::Tab* tab);
  void UpdateTextOverlay(browser::Tab* tab);

  BrowserWorker* worker_;
  int tab_id_ = -1;
  QPlainTextEdit* text_view_;
};

}  // namespace neko::ui
