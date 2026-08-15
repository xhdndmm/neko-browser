#include "neko/ui/web_view.h"

#include "neko/browser/hyperlink.h"
#include "neko/paint/rasterizer.h"
#include "neko/ui/browser_worker.h"

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
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  // Comfortable line step for the scroll-bar arrows / arrow keys. Wheel
  // scrolling is handled separately in wheelEvent().
  verticalScrollBar()->setSingleStep(50);
  // Re-render the visible region whenever the scroll position changes.
  connect(
      verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { viewport()->update(); });

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
  verticalScrollBar()->setValue(verticalScrollBar()->value() - notches * step);
  event->accept();
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
  if (snapshot_.id < 0 || snapshot_.content_type != browser::ContentType::kHtml ||
      snapshot_.page == nullptr) {
    return;
  }
  // The layout tree is in document coordinates; add the scroll offset.  The
  // snapshot keeps the page (and its DOM) alive, so the hit-tested element
  // cannot be freed while we resolve the link.
  const float doc_x = static_cast<float>(viewport_pos.x());
  const float doc_y = static_cast<float>(viewport_pos.y()) + ScrollY();
  const dom::Element* element = snapshot_.page->ElementAt(doc_x, doc_y);
  const std::optional<std::string> target = browser::HyperlinkTarget(element, snapshot_.url);
  if (target.has_value()) {
    worker_->Navigate(tab_id_, QString::fromStdString(*target));
  }
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
  viewport()->update();
}

void WebView::HandleHoverClear()
{
  if (hovered_element_ == nullptr) {
    return;
  }
  hovered_element_ = nullptr;
  if (snapshot_.page != nullptr) {
    snapshot_.page->SetHoveredElement(nullptr);
  }
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
  // Re-layout only when the viewport width changes or the page was just
  // (re)loaded (a fresh page has no layout tree). This keeps scrolling from
  // rebuilding the whole layout tree on every repaint.
  if (laid_out_width_ == width && snapshot_.page->layout_root() != nullptr)
    return;
  snapshot_.page->Layout(static_cast<float>(width));
  laid_out_width_ = width;
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
