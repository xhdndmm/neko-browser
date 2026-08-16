// Neko Browser — Qt6 GUI entry point.
//
// Creates the profile directory, starts the BrowserWorker (which owns the
// single-threaded BrowserController) and shows the main window.

#include "neko/base/logging.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/main_window.h"

#include <QApplication>
#include <QDir>
#include <QString>

namespace {

QString DefaultProfileDir()
{
  const char* env = std::getenv("NEKO_PROFILE");
  if (env != nullptr && *env != '\0')
    return QString::fromUtf8(env);
  QString base = QDir::homePath();
#if defined(_WIN32)
  base += "/AppData/Roaming";
#elif defined(__APPLE__)
  base += "/Library/Application Support";
#else
  base += "/.local/share";
#endif
  return base + "/neko-browser";
}

} // namespace

int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  QApplication::setApplicationName("Neko Browser");

  const QString profile = DefaultProfileDir();
  QDir().mkpath(profile);

  neko::ui::BrowserWorker worker(profile);
  neko::ui::MainWindow window(&worker);
  window.show();
  return app.exec();
}
