// Neko Browser — Qt6 GUI entry point.
//
// Creates the profile directory, starts the BrowserWorker (which owns the
// single-threaded BrowserController) and shows the main window.
//
// Options:
//   --renderer-process   Render HTML in a renderer child process (ADR 0016
//                        M2 "isolated rendering mode"); every tab then runs
//                        its page in its own process, so a crash there cannot
//                        take the browser down.

#include "neko/base/logging.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/main_window.h"

#include <QApplication>
#include <QDir>
#include <QString>
#include <csignal>
#include <cstring>
#include <vector>

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
#ifndef _WIN32
  // A renderer child that dies mid-write must surface as a failed write, not
  // SIGPIPE terminating the browser.
  std::signal(SIGPIPE, SIG_IGN);
#endif
  // The flag has to be parsed before QApplication sees it (Qt would reject an
  // unknown option).
  neko::browser::RendererOptions renderer;
  std::vector<char*> qt_argv;
  qt_argv.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    if (std::strcmp(argv[i], "--renderer-process") == 0) {
      renderer.enabled = true;
      continue;
    }
    qt_argv.push_back(argv[i]);
  }
  int qt_argc = static_cast<int>(qt_argv.size());
  QApplication app(qt_argc, qt_argv.data());
  QApplication::setApplicationName("Neko Browser");

  const QString profile = DefaultProfileDir();
  QDir().mkpath(profile);

  neko::ui::BrowserWorker worker(profile, nullptr, renderer);
  neko::ui::MainWindow window(&worker);
  window.show();
  return app.exec();
}
