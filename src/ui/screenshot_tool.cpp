// GUI screenshot tool (development/validation).
//
//   neko_gui_screenshot [URL|path] [output.png]
//
// Loads the URL in the real GUI stack (BrowserWorker + MainWindow + WebView),
// waits for it to finish, and saves a grab of the window as PNG.  Useful for
// end-to-end visual validation in CI and locally.

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QString>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "neko/ui/browser_worker.h"
#include "neko/ui/main_window.h"

namespace {

QString DefaultProfileDir() {
  const char* env = std::getenv("NEKO_PROFILE");
  if (env != nullptr && *env != '\0') return QString::fromUtf8(env);
  return QString("/tmp/neko-gui-screenshot-profile");
}

}  // namespace

int main(int argc, char** argv) {
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

  // Wait for the GUI to reflect the navigation (tab title / address bar).
  bool ready = false;
  for (int i = 0; i < 200 && !ready; ++i) {
    QCoreApplication::processEvents();
    const auto tab = worker.SnapshotActiveTab();
    ready = tab.id >= 0 && !tab.title.empty();
    if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
  std::printf("saved %s (%dx%d)\n", out.toLocal8Bit().constData(), shot.width(),
              shot.height());
  return 0;
}
