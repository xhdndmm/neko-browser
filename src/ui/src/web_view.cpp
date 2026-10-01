#include "neko/ui/web_view.h"

#include "neko/base/memory.h"
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
#include <memory>
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
  // Re-render the visible region whenever the scroll position changes, and
  // report the new offset to the worker so window.scrollY reads it live.
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
    viewport()->update();
    worker_->SetScrollOffset(tab_id_, value);
  });

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
  // If this refresh replaces the tab's document (navigation), the previous
  // document's memory should return to the OS once its last reference dies.
  // Only *document* replacement triggers this: frame updates (scrolling,
  // animation ticks) are far too frequent for a heap trim.
  std::shared_ptr<renderer::Page> previous_page = snapshot_.page;
  snapshot_ = worker_->SnapshotTab(tab_id_);
  const bool document_replaced = previous_page != nullptr && previous_page != snapshot_.page;
  // Drop the local reference before trimming so the old Page is destroyed
  // first (see base::ReleaseFreeMemory).
  previous_page.reset();

  // A navigation resets the local scroll position; the worker re-renders at
  // the new offset.  The DOM is owned by the worker thread (ADR 0020), so the
  // GUI detects a navigation by the URL rather than by a document version.
  if (snapshot_.url != frame_url_) {
    frame_url_ = snapshot_.url;
    verticalScrollBar()->setValue(0);
  }
  if (document_replaced) {
    base::ReleaseFreeMemory();
  }
  UpdateTextOverlay();
  UpdateScrollRange();
  // Report our viewport: the worker lays the page out for the real size and
  // produces the frame (both execution modes).
  ReportViewport();
  // A page script requested a scroll (window.scrollTo / element.scrollTop):
  // the worker bumped scroll_request_id.  Apply it once the scroll range
  // exists (the worker produces the frame asynchronously); consuming the
  // latch while maximum()==0 would drop the request.
  if (snapshot_.scroll_request_id != applied_scroll_request_id_ &&
      snapshot_.content_type == browser::ContentType::kHtml && verticalScrollBar()->maximum() > 0) {
    applied_scroll_request_id_ = snapshot_.scroll_request_id;
    verticalScrollBar()->setValue(static_cast<int>(snapshot_.pending_scroll_y));
  }
  ApplyFrameCursor();
  viewport()->update();
}

void WebView::ReportViewport()
{
  const int width = std::max(1, viewport()->width());
  const int height = std::max(1, viewport()->height());
  if (reported_viewport_ && width == reported_viewport_w_ && height == reported_viewport_h_) {
    return;
  }
  reported_viewport_w_ = width;
  reported_viewport_h_ = height;
  reported_viewport_ = true;
  worker_->SetViewportSize(tab_id_, width, height);
}

void WebView::ApplyFrameCursor()
{
  viewport()->setCursor(snapshot_.frame_hover_link.empty() ? Qt::ArrowCursor
                                                           : Qt::PointingHandCursor);
}

void WebView::resizeEvent(QResizeEvent* event)
{
  QAbstractScrollArea::resizeEvent(event);
  UpdateScrollRange();
  text_view_->setGeometry(viewport()->rect());
  ReportViewport();
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
  case Qt::Key_Enter:
    key = "Enter";
    code = "Enter";
    return true;
  case Qt::Key_Backspace:
    key = "Backspace";
    code = "Backspace";
    return true;
  case Qt::Key_Space:
    key = " ";
    code = "Space";
    return true;
  case Qt::Key_Escape:
    key = "Escape";
    code = "Escape";
    return true;
  case Qt::Key_Tab:
    key = "Tab";
    code = "Tab";
    return true;
  case Qt::Key_Delete:
    key = "Delete";
    code = "Delete";
    return true;
  case Qt::Key_Up:
    key = "ArrowUp";
    code = "ArrowUp";
    return true;
  case Qt::Key_Down:
    key = "ArrowDown";
    code = "ArrowDown";
    return true;
  case Qt::Key_Left:
    key = "ArrowLeft";
    code = "ArrowLeft";
    return true;
  case Qt::Key_Right:
    key = "ArrowRight";
    code = "ArrowRight";
    return true;
  case Qt::Key_Home:
    key = "Home";
    code = "Home";
    return true;
  case Qt::Key_End:
    key = "End";
    code = "End";
    return true;
  case Qt::Key_PageUp:
    key = "PageUp";
    code = "PageUp";
    return true;
  case Qt::Key_PageDown:
    key = "PageDown";
    code = "PageDown";
    return true;
  default:
    return false;
  }
}

} // namespace

void WebView::keyPressEvent(QKeyEvent* event)
{
  std::string key;
  std::string code;
  bool dispatch = QtKeyToDomKey(event->key(), key, code);
  if (!event->text().isEmpty() && !event->text().at(0).isNull()) {
    key = event->text().toUtf8().toStdString();
    if (code.empty()) {
      code = "Unidentified";
    }
    dispatch = true;
  }
  if (snapshot_.content_type == browser::ContentType::kHtml && dispatch) {
    worker_->DispatchKeyboard(tab_id_,
                              QStringLiteral("keydown"),
                              QString::fromStdString(key),
                              QString::fromStdString(code));
    worker_->DispatchKeyboard(tab_id_,
                              QStringLiteral("keyup"),
                              QString::fromStdString(key),
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
  if (snapshot_.id < 0 || snapshot_.content_type != browser::ContentType::kHtml) {
    return;
  }
  // The worker hit-tests against its own layout (the DOM lives there) and
  // reports the hyperlink under the pointer in the next snapshot; the cursor
  // follows ApplyFrameCursor().
  const float doc_x = static_cast<float>(viewport_pos.x());
  const float doc_y = static_cast<float>(viewport_pos.y()) + ScrollY();
  worker_->DispatchHover(tab_id_, doc_x, doc_y);
}

void WebView::HandleHoverClear()
{
  worker_->DispatchHoverClear(tab_id_);
  viewport()->setCursor(Qt::ArrowCursor);
}

void WebView::UpdateScrollRange()
{
  if (snapshot_.id < 0 || snapshot_.content_type != browser::ContentType::kHtml) {
    verticalScrollBar()->setRange(0, 0);
    return;
  }
  // The worker reports the content height with every frame; the GUI never
  // walks the layout tree.
  const int content_height = static_cast<int>(snapshot_.frame_content_height);
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
  PaintFindHighlight(painter);
}

void WebView::PaintFindHighlight(QPainter& painter)
{
  // Find-in-page (Ctrl+F): a browser-side overlay over the page for the
  // current match, like real browsers (the document itself is untouched).
  // The rectangle arrives in device pixels in document coordinates, so the
  // scroll offset is subtracted here.  NOT IMPLEMENTED: highlighting every
  // match at once (only the current one is drawn).
  if (snapshot_.content_type != browser::ContentType::kHtml || snapshot_.find_match_count <= 0 ||
      snapshot_.find_current_index < 0) {
    return;
  }
  const renderer::FindMatch& match = snapshot_.find_current_match;
  if (match.width <= 0.0F || match.height <= 0.0F) {
    return;
  }
  // FindMatch holds float (device px); QRectF is qreal (double on some
  // platforms).  Convert explicitly: an implicit float -> double conversion
  // would trip -Wdouble-promotion, which is an error in CI.
  const QRectF rect(static_cast<qreal>(match.x),
                    static_cast<qreal>(match.y) - static_cast<qreal>(verticalScrollBar()->value()),
                    static_cast<qreal>(match.width),
                    static_cast<qreal>(match.height));
  painter.fillRect(rect, QColor(255, 214, 0, 128));
  painter.setPen(QColor(180, 140, 0, 200));
  painter.drawRect(rect.adjusted(0, 0, -1, -1));
}

void WebView::PaintHtml(QPainter& painter)
{
  // The worker produces an immutable viewport frame (ADR 0020); the GUI only
  // draws it.  The frame is already rasterized at the current scroll offset,
  // so it is drawn at the origin.  QImage does not copy the pixels and never
  // writes through them; the const_cast only satisfies its API.
  const std::shared_ptr<const browser::RemoteFrame>& frame = snapshot_.frame;
  if (frame != nullptr && frame->width > 0 && frame->height > 0 && !frame->rgba.empty()) {
    const QImage image(const_cast<std::uint8_t*>(frame->rgba.data()),
                       frame->width,
                       frame->height,
                       QImage::Format_RGBA8888);
    painter.drawImage(0, 0, image);
  }
  // Caret overlay: the worker reports the focused element's geometry; the GUI
  // draws the blinking bar itself (blinking never touches the DOM).
  if (snapshot_.has_caret && caret_visible_ && snapshot_.caret_height > 0) {
    const int y = static_cast<int>(snapshot_.caret_y) - verticalScrollBar()->value();
    const int height = static_cast<int>(snapshot_.caret_height);
    if (y + height > 0 && y < viewport()->height()) {
      painter.fillRect(QRect(static_cast<int>(snapshot_.caret_x), y, 1, height), Qt::black);
    }
  }
}

void WebView::OnCaretBlink()
{
  // Only blink while a control holds focus; an idle page never repaints.
  if (!snapshot_.has_caret) {
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
