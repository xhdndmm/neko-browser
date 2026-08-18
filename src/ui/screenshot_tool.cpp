// GUI screenshot tool (development/validation).
//
//   neko_gui_screenshot [URL|path] [output.png]
//
// Loads the URL in the real GUI stack (BrowserWorker + MainWindow + WebView),
// waits for it to finish, and saves a grab of the window as PNG.  Useful for
// end-to-end visual validation in CI and locally.

#include "neko/ui/browser_worker.h"
#include "neko/ui/main_window.h"

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QTabBar>
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

  const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("about:blank");
  const QString out = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("screenshot.png");

  neko::ui::BrowserWorker worker(DefaultProfileDir());
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
    ready = window.TabBarWidget()->count() > 0 && !tab.loading &&
            (tab.page != nullptr || tab.image != nullptr || tab.pdf != nullptr ||
             tab.audio != nullptr || tab.raw_text != nullptr || tab.error != nullptr);
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
