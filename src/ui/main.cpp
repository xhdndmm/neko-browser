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
//   --network-process    Fetch documents through a network child process
//                        (ADR 0016 M3a); DNS/TCP/TLS/HTTP run in the child,
//                        cookies stay in the browser.

#include "neko/base/logging.h"
#include "neko/browser/preferences_keys.h"
#include "neko/storage/preferences.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/i18n.h"
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
  neko::browser::NetworkOptions network;
  std::vector<char*> qt_argv;
  qt_argv.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    if (std::strcmp(argv[i], "--renderer-process") == 0) {
      renderer.enabled = true;
      continue;
    }
    if (std::strcmp(argv[i], "--network-process") == 0) {
      network.enabled = true;
      continue;
    }
    qt_argv.push_back(argv[i]);
  }
  int qt_argc = static_cast<int>(qt_argv.size());
  QApplication app(qt_argc, qt_argv.data());
  QApplication::setApplicationName("Neko Browser");

  const QString profile = DefaultProfileDir();
  QDir().mkpath(profile);

  // Install the UI language before the window (and every widget string) is
  // created.  The choice is a profile preference; empty follows the system
  // locale.  Reading the store directly here is deliberate: the translator
  // must exist before the BrowserWorker/main window are constructed.
  {
    neko::storage::PreferencesStore preferences(profile.toStdString());
    (void)preferences.Load();
    neko::ui::i18n::ApplyLanguage(app, preferences.Get(neko::browser::prefs::kLanguage));
  }

  neko::ui::BrowserWorker worker(profile, nullptr, renderer, network);
  neko::ui::MainWindow window(&worker);
  window.show();
  return app.exec();
}
