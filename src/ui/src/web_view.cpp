#include "neko/ui/web_view.h"

#include "neko/browser/hyperlink.h"
#include "neko/layout/layout_tree.h"
#include "neko/paint/rasterizer.h"
#include "neko/ui/browser_worker.h"

#include <QCursor>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <optional>
#include <string>

namespace neko::ui {

WebView::WebView(BrowserWorker* worker, int tab_id, QWidget* parent)
    : QAbstractScrollArea(parent), worker_(worker), tab_id_(tab_id)
{
  viewport()->setAutoFillBackground(true);
  viewport()->setMouseTracking(true); // receive MouseMove without a pressed button
  setFocusPolicy(Qt::StrongFocus);    // receive keystrokes after a click
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  // Comfortable line step for the scroll-bar arrows / arrow keys. Wheel
  // scrolling is handled separately in wheelEvent().
  verticalScrollBar()->setSingleStep(50);
  // Re-render the visible region whenever the scroll position changes.
  connect(
      verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { viewport()->update(); });

  // Blinking caret: repaint every half second while a control holds focus.
  caret_timer_ = new QTimer(this);
  caret_timer_->setInterval(500);
  connect(caret_timer_, &QTimer::timeout, this, &WebView::OnCaretBlink);
  caret_timer_->start();

  text_view_ = new QPlainTextEdit(viewport());
  text_view_->setReadOnly(true);
  text_view_->setFrameShape(QFrame::NoFrame);
  text_view_->hide();
}

void WebView::Refresh()
{
  snapshot_ = worker_->SnapshotTab(tab_id_);
  // A navigation replaces the page's document (pointer); only then must the
  // hover/active pointers be dropped and the scroll reset to the top.  Using
  // the document pointer (instead of a null layout root) keeps script-driven
  // refreshes from clobbering hover state or the scroll position.
  const dom::Document* doc =
      (snapshot_.content_type == browser::ContentType::kHtml && snapshot_.page != nullptr)
          ? snapshot_.page->document()
          : nullptr;
  if (doc != cached_document_) {
    cached_document_ = doc;
    hovered_element_ = nullptr;
    active_element_ = nullptr;
    verticalScrollBar()->setValue(0);
  }
  UpdateTextOverlay();
  UpdateScrollRange();
  viewport()->update();
}

void WebView::resizeEvent(QResizeEvent* event)
{
  QAbstractScrollArea::resizeEvent(event);
  UpdateScrollRange();
  text_view_->setGeometry(viewport()->rect());
  viewport()->update();
}

void WebView::wheelEvent(QWheelEvent* event)
{
  const int delta = event->angleDelta().y();
  if (delta == 0) {
    event->ignore();
    return;
  }
  // Accumulate the raw delta (eighths of a degree). Most mice deliver ±120
  // per notch, but high-resolution wheels/trackpads deliver smaller amounts
  // that `delta / 120` integer division would truncate to zero.
  wheel_accum_ += delta;
  const int step = std::max(40, viewport()->height() / 8);
  const int notches = wheel_accum_ / 120;
  if (notches == 0) {
    event->accept();
    return;
  }
  wheel_accum_ -= notches * 120;
  // Fire the page's cancelable wheel event (vertical delta in px) so scripts
  // can observe scrolling; the default scroll action still runs below.
  worker_->DispatchWheel(tab_id_, static_cast<double>(notches * step));
  verticalScrollBar()->setValue(verticalScrollBar()->value() - notches * step);
  event->accept();
}

namespace {

// Maps a Qt key to the UI Events (DOM) `key` and `code` strings for the common
// printable and control keys.  Returns false for keys we do not dispatch.
bool QtKeyToDomKey(int qkey, std::string& key, std::string& code)
{
  if (qkey >= Qt::Key_A && qkey <= Qt::Key_Z) {
    const char lower = static_cast<char>('a' + (qkey - Qt::Key_A));
    key = std::string(1, lower);
    code = std::string("Key") + static_cast<char>('A' + (qkey - Qt::Key_A));
    return true;
  }
  if (qkey >= Qt::Key_0 && qkey <= Qt::Key_9) {
    key = std::string(1, static_cast<char>('0' + (qkey - Qt::Key_0)));
    code = std::string("Digit") + key;
    return true;
  }
  switch (qkey) {
  case Qt::Key_Return:
  case Qt::Key_Enter: key = "Enter"; code = "Enter"; return true;
  case Qt::Key_Backspace: key = "Backspace"; code = "Backspace"; return true;
  case Qt::Key_Space: key = " "; code = "Space"; return true;
  case Qt::Key_Escape: key = "Escape"; code = "Escape"; return true;
  case Qt::Key_Tab: key = "Tab"; code = "Tab"; return true;
  case Qt::Key_Delete: key = "Delete"; code = "Delete"; return true;
  case Qt::Key_Up: key = "ArrowUp"; code = "ArrowUp"; return true;
  case Qt::Key_Down: key = "ArrowDown"; code = "ArrowDown"; return true;
  case Qt::Key_Left: key = "ArrowLeft"; code = "ArrowLeft"; return true;
  case Qt::Key_Right: key = "ArrowRight"; code = "ArrowRight"; return true;
  case Qt::Key_Home: key = "Home"; code = "Home"; return true;
  case Qt::Key_End: key = "End"; code = "End"; return true;
  case Qt::Key_PageUp: key = "PageUp"; code = "PageUp"; return true;
  case Qt::Key_PageDown: key = "PageDown"; code = "PageDown"; return true;
  default: return false;
  }
}

} // namespace

void WebView::keyPressEvent(QKeyEvent* event)
{
  std::string key;
  std::string code;
  if (snapshot_.content_type == browser::ContentType::kHtml && QtKeyToDomKey(event->key(), key, code)) {
    worker_->DispatchKeyboard(tab_id_, QStringLiteral("keydown"), QString::fromStdString(key),
                              QString::fromStdString(code));
    worker_->DispatchKeyboard(tab_id_, QStringLiteral("keyup"), QString::fromStdString(key),
                              QString::fromStdString(code));
  }
  QAbstractScrollArea::keyPressEvent(event);
}

bool WebView::viewportEvent(QEvent* event)
{
  // The viewport (not the scroll area) resizes when a scroll bar appears or
  // disappears, so re-sync the scroll range here in addition to resizeEvent().
  if (event->type() == QEvent::Resize) {
    UpdateScrollRange();
  } else if (event->type() == QEvent::MouseMove) {
    const auto* mouse = static_cast<QMouseEvent*>(event);
    HandleHover(mouse->position());
  } else if (event->type() == QEvent::MouseButtonPress) {
    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() == Qt::LeftButton) {
      // Steal keyboard focus so subsequent keystrokes reach the page (typed
      // input, implicit form submission) instead of the address bar.
      setFocus(Qt::MouseFocusReason);
      HandleLinkClick(mouse->position());
      HandleActive(mouse->position());
    }
  } else if (event->type() == QEvent::MouseButtonRelease) {
    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() == Qt::LeftButton) {
      HandleActiveClear();
    }
  } else if (event->type() == QEvent::Leave) {
    HandleHoverClear();
  }
  return QAbstractScrollArea::viewportEvent(event);
}

float WebView::ScrollY() const
{
  return static_cast<float>(verticalScrollBar()->value());
}

void WebView::HandleLinkClick(const QPointF& viewport_pos)
{
  // Dispatch regardless of the cached snapshot's freshness: the worker
  // resolves the hit against its own current tab state, so a click during a
  // navigation (or before a Refresh picked up the new page) is not dropped.
  if (snapshot_.id < 0) {
    return;
  }
  // The layout tree is in document coordinates; add the scroll offset.  The
  // dispatch runs on the worker thread: it hits the element, runs the page's
  // cancelable "click" event, and only then performs the default action
  // (hyperlink navigation) unless a listener called preventDefault().
  const float doc_x = static_cast<float>(viewport_pos.x());
  const float doc_y = static_cast<float>(viewport_pos.y()) + ScrollY();
  worker_->DispatchPointerClick(tab_id_, doc_x, doc_y);
}

void WebView::HandleHover(const QPointF& viewport_pos)
{
  if (snapshot_.id < 0 || snapshot_.content_type != browser::ContentType::kHtml ||
      snapshot_.page == nullptr) {
    return;
  }
  const float doc_x = static_cast<float>(viewport_pos.x());
  const float doc_y = static_cast<float>(viewport_pos.y()) + ScrollY();
  const dom::Element* element = snapshot_.page->ElementAt(doc_x, doc_y);
  if (element == hovered_element_) {
    return;
  }
  hovered_element_ = element;
  snapshot_.page->SetHoveredElement(element);
  // Report the hover change to the page (mouseover/mouseout) on the worker
  // thread, which hit-tests against its own layout.
  worker_->DispatchHover(tab_id_, doc_x, doc_y);
  // Pointing hand over hyperlinks (WHATWG HTML §4.6.5).
  const bool is_link =
      element != nullptr && browser::HyperlinkTarget(element, snapshot_.url).has_value();
  viewport()->setCursor(is_link ? Qt::PointingHandCursor : Qt::ArrowCursor);
  viewport()->update();
}

void WebView::HandleHoverClear()
{
  if (hovered_element_ == nullptr) {
    return;
  }
  hovered_element_ = nullptr;
  viewport()->setCursor(Qt::ArrowCursor);
  if (snapshot_.page != nullptr) {
    snapshot_.page->SetHoveredElement(nullptr);
  }
  worker_->DispatchHoverClear(tab_id_);
  viewport()->update();
}

void WebView::HandleActive(const QPointF& viewport_pos)
{
  if (snapshot_.id < 0 || snapshot_.content_type != browser::ContentType::kHtml ||
      snapshot_.page == nullptr) {
    return;
  }
  const float doc_x = static_cast<float>(viewport_pos.x());
  const float doc_y = static_cast<float>(viewport_pos.y()) + ScrollY();
  const dom::Element* element = snapshot_.page->ElementAt(doc_x, doc_y);
  if (element == active_element_) {
    return;
  }
  active_element_ = element;
  snapshot_.page->SetActiveElement(element);
  viewport()->update();
}

void WebView::HandleActiveClear()
{
  if (active_element_ == nullptr) {
    return;
  }
  active_element_ = nullptr;
  if (snapshot_.page != nullptr) {
    snapshot_.page->SetActiveElement(nullptr);
  }
  viewport()->update();
}

void WebView::EnsureLayout(int width)
{
  if (snapshot_.page == nullptr)
    return;
  // Re-layout only when the viewport size changes or the page was just
  // (re)loaded (a fresh page has no layout tree). This keeps scrolling from
  // rebuilding the whole layout tree on every repaint.
  const int viewport_height = std::max(1, viewport()->height());
  if (laid_out_width_ == width && laid_out_height_ == viewport_height &&
      snapshot_.page->layout_root() != nullptr)
    return;
  snapshot_.page->Layout(static_cast<float>(width), static_cast<float>(viewport_height));
  laid_out_width_ = width;
  laid_out_height_ = viewport_height;
}

void WebView::UpdateScrollRange()
{
  if (snapshot_.id < 0 || snapshot_.content_type != browser::ContentType::kHtml ||
      snapshot_.page == nullptr) {
    verticalScrollBar()->setRange(0, 0);
    return;
  }
  const int viewport_width = std::max(1, viewport()->width());
  EnsureLayout(viewport_width);
  const int content_height = static_cast<int>(snapshot_.page->ContentHeight());
  const int viewport_height = std::max(1, viewport()->height());
  verticalScrollBar()->setRange(0, std::max(0, content_height - viewport_height));
}

void WebView::UpdateTextOverlay()
{
  if (snapshot_.id < 0) {
    text_view_->hide();
    return;
  }
  switch (snapshot_.content_type) {
  case browser::ContentType::kPdf: {
    if (snapshot_.pdf == nullptr) {
      text_view_->hide();
      return;
    }
    QString text;
    for (const pdf::PdfPage& page : snapshot_.pdf->pages) {
      text += QString("----- Page %1 (%2x%3) -----\n")
                  .arg(page.index + 1)
                  .arg(page.width)
                  .arg(page.height);
      text += QString::fromUtf8(page.text.c_str());
      text += "\n\n";
    }
    text_view_->setPlainText(text);
    text_view_->show();
    break;
  }
  case browser::ContentType::kAudio:
    if (snapshot_.audio == nullptr) {
      text_view_->hide();
      return;
    }
    text_view_->setPlainText(
        QString("WAV audio\n  sample rate : %1 Hz\n  channels    : %2\n"
                "  bit depth   : %3\n  samples     : %4\n  duration    : %5 s\n\n"
                "Playback is not implemented yet (see src/media).")
            .arg(snapshot_.audio->sample_rate)
            .arg(snapshot_.audio->channels)
            .arg(snapshot_.audio->bits_per_sample)
            .arg(static_cast<qulonglong>(snapshot_.audio->samples.size()))
            .arg(snapshot_.audio->duration_seconds(), 0, 'f', 2));
    text_view_->show();
    break;
  case browser::ContentType::kText:
  case browser::ContentType::kOther:
    if (snapshot_.raw_text == nullptr) {
      text_view_->hide();
      return;
    }
    text_view_->setPlainText(QString::fromUtf8(snapshot_.raw_text->c_str()));
    text_view_->show();
    break;
  case browser::ContentType::kError:
    text_view_->setPlainText(
        QString("Error\n\n%1\n\n%2")
            .arg(QString::fromUtf8(snapshot_.url.c_str()))
            .arg(QString::fromUtf8(snapshot_.error != nullptr ? snapshot_.error->c_str() : "")));
    text_view_->show();
    break;
  default:
    text_view_->hide();
    break;
  }
}

void WebView::paintEvent(QPaintEvent*)
{
  QPainter painter(viewport());
  painter.fillRect(viewport()->rect(), Qt::white);
  if (snapshot_.id < 0)
    return;
  switch (snapshot_.content_type) {
  case browser::ContentType::kHtml:
    PaintHtml(painter);
    break;
  case browser::ContentType::kImage:
    PaintImage(painter);
    break;
  default:
    break; // text modes use the QPlainTextEdit overlay
  }
}

void WebView::PaintHtml(QPainter& painter)
{
  if (snapshot_.page == nullptr)
    return;
  const int viewport_width = std::max(1, viewport()->width());
  const int viewport_height = std::max(1, viewport()->height());
  EnsureLayout(viewport_width);

  const int scroll = verticalScrollBar()->value();
  const std::uint64_t layout_version = snapshot_.page->layout_version();

  // Full repaint when the cache is missing, the viewport resized or the
  // page's content changed (navigation, style/DOM mutation, image load).
  const bool need_full = !raster_cache_.has_value() || cached_width_ != viewport_width ||
                         cached_height_ != viewport_height ||
                         cached_layout_version_ != layout_version;
  if (need_full) {
    raster_cache_.emplace(viewport_width, viewport_height);
    // Reuse the cached buffer; Page::RasterizeFull pulls the display list
    // from its internal cache (parallel band rasterization on the
    // controller's pool when the viewport is big enough).
    snapshot_.page->RasterizeFull(*raster_cache_, static_cast<float>(scroll), &worker_->pool());
    cached_width_ = viewport_width;
    cached_height_ = viewport_height;
    cached_scroll_ = scroll;
    cached_layout_version_ = layout_version;
  } else if (scroll != cached_scroll_) {
    // Scroll blit: shift the cached buffer by the scroll delta, then
    // re-rasterize only the newly exposed band.
    const int delta = cached_scroll_ - scroll; // screen-space content shift
    raster_cache_->ShiftRows(delta);
    int band_y0 = 0;
    int band_y1 = 0;
    if (delta > 0) {
      band_y0 = 0;
      band_y1 = delta; // content moved down; top band exposed
    } else if (delta < 0) {
      band_y0 = viewport_height + delta;
      band_y1 = viewport_height; // content moved up; bottom band exposed
    }
    if (band_y1 > band_y0) {
      snapshot_.page->RasterizeInto(*raster_cache_, band_y0, band_y1, static_cast<float>(scroll));
    }
    cached_scroll_ = scroll;
  }

  const std::vector<uint8_t>& pixels = raster_cache_->pixels();
  if (pixels.empty())
    return;
  QImage image(const_cast<uint8_t*>(pixels.data()),
               raster_cache_->width(),
               raster_cache_->height(),
               QImage::Format_RGBA8888);
  painter.drawImage(0, 0, image);
  PaintCaret(painter);
}

// Finds the caret point for |target|: the end of its first laid-out text run
// (document coordinates, before scroll).
bool FindCaretPosition(const layout::LayoutBox& box, const dom::Element* target, float& x, float& y,
                       float& h)
{
  for (const layout::Line& line : box.lines) {
    for (const layout::TextRun& run : line.runs) {
      if (run.element == target) {
        x = run.x + run.width;
        y = run.y;
        h = line.height;
        return true;
      }
    }
    for (const layout::InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr && FindCaretPosition(*ib.block_box, target, x, y, h)) {
        return true;
      }
    }
  }
  for (const auto& child : box.children) {
    if (FindCaretPosition(*child, target, x, y, h)) {
      return true;
    }
  }
  for (const auto& f : box.floats) {
    if (FindCaretPosition(*f, target, x, y, h)) {
      return true;
    }
  }
  return false;
}

void WebView::PaintCaret(QPainter& painter)
{
  if (!caret_visible_ || snapshot_.content_type != browser::ContentType::kHtml ||
      snapshot_.page == nullptr || snapshot_.page->layout_root() == nullptr) {
    return;
  }
  const dom::Element* focused = snapshot_.page->FocusedElement();
  if (focused == nullptr || focused->tag_name() != "input") {
    return;
  }
  float x = 0;
  float y = 0;
  float h = 0;
  if (!FindCaretPosition(*snapshot_.page->layout_root(), focused, x, y, h)) {
    return;
  }
  const int scroll = verticalScrollBar()->value();
  painter.fillRect(QRect(static_cast<int>(x), static_cast<int>(y - static_cast<float>(scroll)), 1,
                         std::max(1, static_cast<int>(h))),
                   QColor(0, 0, 0));
}

void WebView::OnCaretBlink()
{
  // Only blink while a control holds focus; an idle page never repaints.
  if (snapshot_.page == nullptr || snapshot_.page->FocusedElement() == nullptr) {
    caret_visible_ = true;
    return;
  }
  caret_visible_ = !caret_visible_;
  viewport()->update();
}

void WebView::PaintImage(QPainter& painter)
{
  if (snapshot_.image == nullptr || snapshot_.image->rgba.empty())
    return;
  QImage image(snapshot_.image->rgba.data(),
               snapshot_.image->width,
               snapshot_.image->height,
               QImage::Format_RGBA8888);
  // Scale down to fit while keeping the aspect ratio.
  const QImage scaled =
      image.scaled(viewport()->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  painter.drawImage((viewport()->width() - scaled.width()) / 2,
                    (viewport()->height() - scaled.height()) / 2,
                    scaled);
}

} // namespace neko::ui
