#include "neko/ui/web_view.h"

#include <QEvent>
#include <QImage>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "neko/paint/rasterizer.h"
#include "neko/ui/browser_worker.h"

namespace neko::ui {

WebView::WebView(BrowserWorker* worker, int tab_id, QWidget* parent)
    : QAbstractScrollArea(parent), worker_(worker), tab_id_(tab_id) {
  viewport()->setAutoFillBackground(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  // Comfortable line step for the scroll-bar arrows / arrow keys. Wheel
  // scrolling is handled separately in wheelEvent().
  verticalScrollBar()->setSingleStep(50);
  // Re-render the visible region whenever the scroll position changes.
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
    viewport()->update();
  });

  text_view_ = new QPlainTextEdit(viewport());
  text_view_->setReadOnly(true);
  text_view_->setFrameShape(QFrame::NoFrame);
  text_view_->hide();
}

browser::Tab* WebView::CurrentTab() const {
  return worker_->controller().FindTab(tab_id_);
}

void WebView::Refresh() {
  browser::Tab* tab = CurrentTab();
  // A freshly loaded page has no layout tree yet; treat it as a new
  // navigation and return to the top so we don't carry over the previous
  // page's scroll offset.
  if (tab != nullptr && tab->content_type == browser::ContentType::kHtml &&
      tab->page.layout_root() == nullptr) {
    verticalScrollBar()->setValue(0);
  }
  UpdateTextOverlay(tab);
  UpdateScrollRange(tab);
  viewport()->update();
}

void WebView::resizeEvent(QResizeEvent* event) {
  QAbstractScrollArea::resizeEvent(event);
  UpdateScrollRange(CurrentTab());
  text_view_->setGeometry(viewport()->rect());
  viewport()->update();
}

void WebView::wheelEvent(QWheelEvent* event) {
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

bool WebView::viewportEvent(QEvent* event) {
  // The viewport (not the scroll area) resizes when a scroll bar appears or
  // disappears, so re-sync the scroll range here in addition to resizeEvent().
  if (event->type() == QEvent::Resize) {
    UpdateScrollRange(CurrentTab());
  }
  return QAbstractScrollArea::viewportEvent(event);
}

float WebView::ScrollY() const {
  return static_cast<float>(verticalScrollBar()->value());
}

void WebView::EnsureLayout(browser::Tab* tab, int width) {
  if (tab == nullptr || tab->content_type != browser::ContentType::kHtml) return;
  // Re-layout only when the viewport width changes or the page was just
  // (re)loaded (a fresh page has no layout tree). This keeps scrolling from
  // rebuilding the whole layout tree on every repaint.
  if (laid_out_width_ == width && tab->page.layout_root() != nullptr) return;
  tab->page.Layout(static_cast<float>(width));
  laid_out_width_ = width;
}

void WebView::UpdateScrollRange(browser::Tab* tab) {
  if (tab == nullptr || tab->content_type != browser::ContentType::kHtml) {
    verticalScrollBar()->setRange(0, 0);
    return;
  }
  const int viewport_width = std::max(1, viewport()->width());
  EnsureLayout(tab, viewport_width);
  const int content_height = static_cast<int>(tab->page.ContentHeight());
  const int viewport_height = std::max(1, viewport()->height());
  verticalScrollBar()->setRange(0, std::max(0, content_height - viewport_height));
}

void WebView::UpdateTextOverlay(browser::Tab* tab) {
  if (tab == nullptr) {
    text_view_->hide();
    return;
  }
  switch (tab->content_type) {
    case browser::ContentType::kPdf: {
      QString text;
      for (const pdf::PdfPage& page : tab->pdf.pages) {
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
      text_view_->setPlainText(
          QString("WAV audio\n  sample rate : %1 Hz\n  channels    : %2\n"
                  "  bit depth   : %3\n  samples     : %4\n  duration    : %5 s\n\n"
                  "Playback is not implemented yet (see src/media).")
              .arg(tab->audio.sample_rate)
              .arg(tab->audio.channels)
              .arg(tab->audio.bits_per_sample)
              .arg(static_cast<qulonglong>(tab->audio.samples.size()))
              .arg(tab->audio.duration_seconds(), 0, 'f', 2));
      text_view_->show();
      break;
    case browser::ContentType::kText:
    case browser::ContentType::kOther:
      text_view_->setPlainText(QString::fromUtf8(tab->raw_text.c_str()));
      text_view_->show();
      break;
    case browser::ContentType::kError:
      text_view_->setPlainText(QString("Error\n\n%1\n\n%2")
                                   .arg(QString::fromUtf8(tab->url.c_str()))
                                   .arg(QString::fromUtf8(tab->error.c_str())));
      text_view_->show();
      break;
    default:
      text_view_->hide();
      break;
  }
}

void WebView::paintEvent(QPaintEvent*) {
  QPainter painter(viewport());
  painter.fillRect(viewport()->rect(), Qt::white);
  browser::Tab* tab = CurrentTab();
  if (tab == nullptr) return;
  switch (tab->content_type) {
    case browser::ContentType::kHtml:
      PaintHtml(painter, tab);
      break;
    case browser::ContentType::kImage:
      PaintImage(painter, tab);
      break;
    default:
      break;  // text modes use the QPlainTextEdit overlay
  }
}

void WebView::PaintHtml(QPainter& painter, browser::Tab* tab) {
  const int viewport_width = std::max(1, viewport()->width());
  const int viewport_height = std::max(1, viewport()->height());
  EnsureLayout(tab, viewport_width);
  const paint::Rasterizer raster =
      tab->page.Rasterize(viewport_width, viewport_height, ScrollY());
  if (raster.pixels().empty()) return;
  QImage image(raster.pixels().data(), raster.width(), raster.height(),
               QImage::Format_RGBA8888);
  painter.drawImage(0, 0, image);
}

void WebView::PaintImage(QPainter& painter, browser::Tab* tab) {
  if (tab->image.empty()) return;
  QImage image(tab->image.rgba.data(), tab->image.width, tab->image.height,
               QImage::Format_RGBA8888);
  // Scale down to fit while keeping the aspect ratio.
  const QImage scaled =
      image.scaled(viewport()->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  painter.drawImage((viewport()->width() - scaled.width()) / 2,
                    (viewport()->height() - scaled.height()) / 2, scaled);
}

}  // namespace neko::ui
