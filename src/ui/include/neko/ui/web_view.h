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
// The tab is identified by id.  Each Refresh() (driven by StateChanged())
// re-reads a thread-safe TabSnapshot from the worker; paint/wheel/resize
// events use the last snapshot, which owns its payload through shared
// handles — so closing or navigating the tab can never invalidate what this
// view is rendering.
class WebView : public QAbstractScrollArea {
  Q_OBJECT
 public:
  WebView(BrowserWorker* worker, int tab_id, QWidget* parent = nullptr);

  void Refresh();  // re-read the tab snapshot; re-render and resync overlays
  int tab_id() const { return tab_id_; }

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  bool viewportEvent(QEvent* event) override;

 private:
  void PaintHtml(QPainter& painter);
  void PaintImage(QPainter& painter);
  void UpdateTextOverlay();
  void EnsureLayout(int width);
  void UpdateScrollRange();
  void HandleLinkClick(const QPointF& viewport_pos);
  float ScrollY() const;

  BrowserWorker* worker_;
  int tab_id_ = -1;
  // Last consistent copy of the tab's renderable state; GUI-thread only.
  browser::TabSnapshot snapshot_;
  QPlainTextEdit* text_view_;
  int laid_out_width_ = -1;  // viewport width the page was last laid out at
  int wheel_accum_ = 0;      // fractional wheel delta (eighths of a degree)
};

}  // namespace neko::ui
