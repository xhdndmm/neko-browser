#pragma once

#include <QAbstractScrollArea>

#include "neko/browser/browser_controller.h"

class QPlainTextEdit;

namespace neko::ui {

class BrowserWorker;

// Paints the content of one tab: rendered HTML (through the engine
// pipeline), decoded images, PDF/audio/text summaries or error messages.
//
// HTML content is rendered at the current viewport width and scrolled with a
// vertical scroll bar driven by the page's total content height.
//
// The tab is identified by id and resolved through the controller each time
// so that closing a tab (which may move other tabs in the controller's
// vector) can never leave this view holding a stale pointer.
class WebView : public QAbstractScrollArea {
  Q_OBJECT
 public:
  WebView(BrowserWorker* worker, int tab_id, QWidget* parent = nullptr);

  void Refresh();  // re-layout/re-render and resync the text overlay
  int tab_id() const { return tab_id_; }

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  bool viewportEvent(QEvent* event) override;

 private:
  browser::Tab* CurrentTab() const;
  void PaintHtml(QPainter& painter, browser::Tab* tab);
  void PaintImage(QPainter& painter, browser::Tab* tab);
  void UpdateTextOverlay(browser::Tab* tab);
  void EnsureLayout(browser::Tab* tab, int width);
  void UpdateScrollRange(browser::Tab* tab);
  float ScrollY() const;

  BrowserWorker* worker_;
  int tab_id_ = -1;
  QPlainTextEdit* text_view_;
  int laid_out_width_ = -1;  // viewport width the page was last laid out at
  int wheel_accum_ = 0;      // fractional wheel delta (eighths of a degree)
};

}  // namespace neko::ui
