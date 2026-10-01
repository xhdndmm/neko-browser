#pragma once

#include "neko/browser/browser_controller.h"

#include <QAbstractScrollArea>
#include <QTimer>
#include <cstdint>
#include <memory>
#include <optional>

class QPlainTextEdit;
class QKeyEvent;
class QMenu;

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
//
// Rendering performance: the viewport raster is cached and reused across
// paint events.  Pure scrolls shift the cached buffer (memmove) and
// re-rasterize only the newly exposed rows; full repaints happen only when
// the viewport resizes or the page's layout version changes.
class WebView : public QAbstractScrollArea
{
  Q_OBJECT
public:
  WebView(BrowserWorker* worker, int tab_id, QWidget* parent = nullptr);

  void Refresh(); // re-read the tab snapshot; re-render and resync overlays
  int tab_id() const
  {
    return tab_id_;
  }

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool viewportEvent(QEvent* event) override;

private:
  void PaintHtml(QPainter& painter);
  void PaintImage(QPainter& painter);
  // Find-in-page (Ctrl+F): draws the current match's highlight over the page.
  void PaintFindHighlight(QPainter& painter);
  void OnCaretBlink();
  void UpdateTextOverlay();
  void UpdateScrollRange();
  void HandleLinkClick(const QPointF& viewport_pos);
  void HandleHover(const QPointF& viewport_pos);
  void HandleHoverClear();
  float ScrollY() const;
  // Reports the viewport size to the worker (which lays the page out for it
  // and produces the frame) and applies the worker-reported hover cursor.
  void ReportViewport();
  void ApplyFrameCursor();

public:
  // Builds the page context menu for a viewport position (hyperlink, image
  // and navigation entries).  The caller pops it up; kept separate from
  // ShowPageContextMenu so tests can inspect the entries without opening a
  // modal menu.  Returns nullptr when there is no active HTML tab.
  QMenu* CreatePageContextMenu(const QPoint& viewport_pos);

private:
  void ShowPageContextMenu(const QPoint& global_pos, const QPoint& viewport_pos);

  BrowserWorker* worker_;
  int tab_id_ = -1;
  // Last consistent copy of the tab's renderable state; GUI-thread only.
  browser::TabSnapshot snapshot_;
  QPlainTextEdit* text_view_;
  int wheel_accum_ = 0; // fractional wheel delta (eighths of a degree)
  // URL the last frame belonged to; a change means a navigation, which resets
  // the local scroll position (the worker re-renders at the new offset).
  std::string frame_url_;
  // Viewport size already reported to the worker (-1 = not yet).
  int reported_viewport_w_ = -1;
  int reported_viewport_h_ = -1;
  bool reported_viewport_ = false;
  // Last applied script-requested scroll latch id.  A fresh value (worker bumped
  // scroll_request_id) moves the scroll bar to the requested offset exactly once.
  std::uint64_t applied_scroll_request_id_ = 0;
  // Blinking caret for the focused element (GUI thread).  The blink timer
  // flips visibility only while a control holds focus, so an idle page never
  // triggers repaints.
  bool caret_visible_ = true;
  QTimer* caret_timer_ = nullptr;
};

} // namespace neko::ui
