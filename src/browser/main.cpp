#include <iostream>
#include <string>

#include "neko/base/logging.h"
#include "neko/base/version.h"
#include "neko/browser/browser_options.h"

int main(int argc, char** argv) {
  const neko::browser::ParseResult parsed = neko::browser::ParseCommandLine(argc, argv);

  switch (parsed.action) {
    case neko::browser::ParseResult::Action::kHelp:
      std::cout << neko::browser::UsageText();
      return 0;
    case neko::browser::ParseResult::Action::kVersion:
      std::cout << neko::base::GetProjectName() << ' ' << neko::base::GetVersionString() << '\n';
      return 0;
    case neko::browser::ParseResult::Action::kError:
      std::cerr << "error: " << parsed.error_message << "\n\n" << neko::browser::UsageText();
      return 2;
    case neko::browser::ParseResult::Action::kRun:
      break;
  }

  neko::base::Logger::Instance().SetLevel(parsed.options.log_level);
  NEKO_LOG_INFO("neko-browser " + std::string(neko::base::GetVersionString()) +
                " starting (Phase 0 bootstrap)");
  NEKO_LOG_DEBUG("headless=" + std::string(parsed.options.headless ? "true" : "false"));

  // Phase 0: the engine pipeline (network -> html -> css -> layout -> paint)
  // is not implemented yet.  Report it explicitly instead of faking success.
  if (parsed.options.url.has_value()) {
    NEKO_LOG_INFO("requested URL: " + parsed.options.url.value());
    std::cout << "NOT IMPLEMENTED: URL navigation arrives in Phase 2 (networking).\n";
  }
  if (parsed.options.dump_dom) {
    std::cout << "NOT IMPLEMENTED: --dump-dom requires the HTML/DOM pipeline (Phase 3).\n";
  }
  if (parsed.options.screenshot_path.has_value()) {
    std::cout << "NOT IMPLEMENTED: --screenshot requires the rendering pipeline (Phase 6).\n";
  }

  NEKO_LOG_INFO("bootstrap OK: CLI parsed, logging online");
  return 0;
}
