#include "neko/ui/web_view.h"

#include <QImage>
#include <QPainter>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include "neko/paint/rasterizer.h"
#include "neko/ui/browser_worker.h"

namespace neko::ui {

WebView::WebView(BrowserWorker* worker, int tab_id, QWidget* parent)
    : QWidget(parent), worker_(worker), tab_id_(tab_id) {
  setAutoFillBackground(true);
  text_view_ = new QPlainTextEdit(this);
  text_view_->setReadOnly(true);
  text_view_->setFrameShape(QFrame::NoFrame);
  text_view_->hide();
}

browser::Tab* WebView::CurrentTab() const {
  return worker_->controller().FindTab(tab_id_);
}

void WebView::Refresh() {
  UpdateTextOverlay(CurrentTab());
  update();
}

void WebView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  text_view_->setGeometry(rect());
  update();
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
  QPainter painter(this);
  painter.fillRect(rect(), Qt::white);
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
  const int viewport_width = std::max(1, width());
  const int viewport_height = std::max(1, height());
  // Layout at the current viewport width and rasterize (software).
  tab->page.Layout(static_cast<float>(viewport_width));
  const paint::Rasterizer raster = tab->page.Rasterize(viewport_width, viewport_height);
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
  const QImage scaled = image.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  painter.drawImage((width() - scaled.width()) / 2, (height() - scaled.height()) / 2,
                    scaled);
}

}  // namespace neko::ui
