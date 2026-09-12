// GUI screenshot tool (development/validation).
//
//   neko_gui_screenshot [--renderer-process] [URL|path] [output.png]
//
// Loads the URL in the real GUI stack (BrowserWorker + MainWindow + WebView),
// waits for it to finish, and saves a grab of the window as PNG.  Useful for
// end-to-end visual validation in CI and locally.  With --renderer-process the
// page is rendered by a renderer child (ADR 0016 M2).

#include "neko/ui/browser_worker.h"
#include "neko/ui/main_window.h"
#include "neko/ui/web_view.h"

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QTabBar>
#include <QWidget>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

QString DefaultProfileDir()
{
  const char* env = std::getenv("NEKO_PROFILE");
  if (env != nullptr && *env != '\0')
    return QString::fromUtf8(env);
  return QString("/tmp/neko-gui-screenshot-profile");
}

} // namespace

int main(int argc, char** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  QApplication::setApplicationName("Neko Browser Screenshot");

  neko::browser::RendererOptions renderer;
  QString url = QStringLiteral("about:blank");
  QString out = QStringLiteral("screenshot.png");
  bool have_url = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--renderer-process") {
      renderer.enabled = true;
      continue;
    }
    if (!have_url) {
      url = QString::fromLocal8Bit(argv[i]);
      have_url = true;
      continue;
    }
    out = QString::fromLocal8Bit(argv[i]);
  }
  neko::ui::BrowserWorker worker(DefaultProfileDir(), nullptr, renderer);
  neko::ui::MainWindow window(&worker);
  window.resize(1100, 800);
  window.show();

  worker.NavigateActive(url);

  // Wait until navigation and synchronous subresource loading complete, then
  // let the queued StateChanged refresh publish the final page snapshot to the
  // WebView. A tab title alone appears before images and stylesheets finish.
  bool ready = false;
  for (int i = 0; i < 200 && !ready; ++i) {
    QCoreApplication::processEvents();
    const neko::browser::TabSnapshot tab = worker.SnapshotActiveTab();
    const bool has_content = tab.page != nullptr || tab.image != nullptr || tab.pdf != nullptr ||
                             tab.audio != nullptr || tab.raw_text != nullptr ||
                             tab.error != nullptr;
    if (has_content) {
      ready = window.TabBarWidget()->count() > 0 && !tab.loading;
    } else if (tab.remote && tab.remote_frame != nullptr) {
      // Renderer mode: the first frame is rasterized for the controller's
      // default viewport; wait until the child has been re-laid out for the
      // view's real viewport so the grab shows the final image.
      const neko::ui::WebView* view = window.ActiveView();
      ready = window.TabBarWidget()->count() > 0 && !tab.loading && view != nullptr &&
              tab.remote_frame->width == view->viewport()->width() &&
              tab.remote_frame->height == view->viewport()->height();
    }
    if (!ready)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  const QPixmap shot = window.grab();
  if (shot.isNull()) {
    std::fprintf(stderr, "failed to grab window\n");
    return 1;
  }
  if (!shot.save(out)) {
    std::fprintf(stderr, "failed to save %s\n", out.toLocal8Bit().constData());
    return 1;
  }
  std::printf("saved %s (%dx%d)\n", out.toLocal8Bit().constData(), shot.width(), shot.height());
  return 0;
}
