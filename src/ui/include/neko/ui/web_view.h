#pragma once

#include "neko/browser/browser_controller.h"
#include "neko/compositor/compositor.h"
#include "neko/paint/rasterizer.h"

#include <QAbstractScrollArea>
#include <QTimer>
#include <cstdint>
#include <memory>
#include <optional>

class QPlainTextEdit;
class QKeyEvent;

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
  // Recomputes the caret overlay layer (layer 1) from the focused element
  // and the current scroll.  Returns true when the caret's screen rect or
  // visibility changed since the last call.
  bool UpdateCaretLayer();
  void OnCaretBlink();
  void UpdateTextOverlay();
  void EnsureLayout(int width);
  void UpdateScrollRange();
  void HandleLinkClick(const QPointF& viewport_pos);
  void HandleHover(const QPointF& viewport_pos);
  void HandleHoverClear();
  void HandleActive(const QPointF& viewport_pos);
  void HandleActiveClear();
  float ScrollY() const;

  BrowserWorker* worker_;
  int tab_id_ = -1;
  // Last consistent copy of the tab's renderable state; GUI-thread only.
  browser::TabSnapshot snapshot_;
  QPlainTextEdit* text_view_;
  int laid_out_width_ = -1;  // viewport width the page was last laid out at
  int laid_out_height_ = -1; // viewport height the page was last laid out at
  int wheel_accum_ = 0;      // fractional wheel delta (eighths of a degree)
  // Element last reported as hovered/active (GUI thread); reset when the
  // document is replaced by a navigation so the state re-resolves from scratch.
  const dom::Element* hovered_element_ = nullptr;
  const dom::Element* active_element_ = nullptr;
  // Document the hover/active state was resolved against; a change signals a
  // navigation (the document pointer is replaced), requiring a scroll reset.
  const dom::Document* cached_document_ = nullptr;

  // Blinking caret for the focused element (GUI thread).  The blink timer
  // flips visibility only while a control holds focus, so an idle page never
  // triggers repaints.
  bool caret_visible_ = true;
  QTimer* caret_timer_ = nullptr;

  // Cached viewport raster + the state it was produced for (GUI thread).
  std::optional<paint::Rasterizer> raster_cache_;
  int cached_width_ = -1;
  int cached_height_ = -1;
  int cached_scroll_ = -1;
  std::uint64_t cached_layout_version_ = 0;

  // Presentation compositor (ADR 0015): layer 0 = rasterized page, layer 1 =
  // the caret overlay.  The GUI paints the compositor's output surface.
  std::unique_ptr<compositor::Compositor> compositor_;
  // Last computed caret overlay rect/visibility (output coordinates).
  int caret_x_ = -1;
  int caret_y_ = -1;
  int caret_h_ = 0;
  bool caret_drawn_ = false;
};

} // namespace neko::ui
