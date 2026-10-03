// neko_gui_perf — steady-state smoothness measurement for the GUI path.
//
// The headless CLI measures a load, and its pump has no frame cadence, so the
// rAF callbacks accumulated during a load are drained in one turn; that is what
// the load-time "longest pump turn" numbers are made of.  This tool drives the
// real cadence instead (pump every 50 ms, like MainWindow's QTimer) for a bounded
// number of seconds and reports the same JSON line as --perf-report, so the
// interactive path can be judged by frame production and frame intervals rather
// than by a load-time batch.
//
// Usage:
//   neko_gui_perf <url> [--seconds N] [--interval-ms N] [--raf-per-turn N]
//                      [--profile <dir>] [--quiet]
//
// Nothing here gates on time: the numbers are evidence (TESTING.md, "时间相关
// 测试"; AGENTS.md 44).  Run it before and after a change and compare.

#include "neko/browser/perf_report.h"
#include "neko/network/http_client.h"
#include "neko/ui/browser_worker.h"
#include "neko/ui/steady_state.h"

#include <QApplication>
#include <QString>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace {

void PrintUsage()
{
  std::fprintf(stderr,
               "usage: neko_gui_perf <url> [--seconds N] [--interval-ms N]\n"
               "                     [--raf-per-turn N] [--profile <dir>] [--quiet]\n"
               "\n"
               "Drives the GUI's own pump cadence for N seconds and prints one JSON\n"
               "line of frame and pump measurements (see docs/development/perf-baselines.md).\n"
               "  --seconds N        how long to drive (default 5)\n"
               "  --interval-ms N    pump cadence (default 50, the GUI timer's)\n"
               "  --raf-per-turn N   per-turn requestAnimationFrame budget (0 = engine default)\n"
               "  --quiet            print only the JSON line\n");
}

std::string DefaultProfileDir()
{
  const char* env = std::getenv("NEKO_PROFILE");
  if (env != nullptr && *env != '\0') {
    return env;
  }
  return "/tmp/neko-gui-perf-profile";
}

} // namespace

int main(int argc, char** argv)
{
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  QApplication::setApplicationName("Neko Browser Perf");

  QString url;
  bool have_url = false;
  bool quiet = false;
  std::string profile = DefaultProfileDir();
  neko::ui::SteadyStateOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next_value = [&](const char* what) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", what);
        return std::nullopt;
      }
      return std::string(argv[++i]);
    };
    if (arg == "--seconds") {
      const auto value = next_value("--seconds");
      if (!value.has_value()) {
        return 2;
      }
      options.duration = std::chrono::milliseconds(std::stoll(*value) * 1000);
      continue;
    }
    if (arg == "--interval-ms") {
      const auto value = next_value("--interval-ms");
      if (!value.has_value()) {
        return 2;
      }
      options.interval = std::chrono::milliseconds(std::stoll(*value));
      continue;
    }
    if (arg == "--raf-per-turn") {
      const auto value = next_value("--raf-per-turn");
      if (!value.has_value()) {
        return 2;
      }
      const long long limit = std::stoll(*value);
      if (limit < 0) {
        std::fprintf(stderr, "error: --raf-per-turn must be >= 0\n");
        return 2;
      }
      options.raf_per_turn = static_cast<std::size_t>(limit);
      continue;
    }
    if (arg == "--profile") {
      const auto value = next_value("--profile");
      if (!value.has_value()) {
        return 2;
      }
      profile = *value;
      continue;
    }
    if (arg == "--quiet") {
      quiet = true;
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    }
    if (!have_url) {
      url = QString::fromLocal8Bit(argv[i]);
      have_url = true;
      continue;
    }
    std::fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
    PrintUsage();
    return 2;
  }

  if (!have_url) {
    PrintUsage();
    return 2;
  }

  // A worker without a window: NewTab gives the controller the active tab the pump
  // needs, and leaving MainWindow out means no hidden 50 ms timer competes with
  // this harness's own cadence -- the measured turn count is then exactly the
  // cadence this tool drove.
  neko::ui::BrowserWorker worker(QString::fromStdString(profile), nullptr, {});
  const auto wall_start = std::chrono::steady_clock::now();
  worker.NewTab(url, /*activate=*/true);
  const neko::ui::SteadyStateResult state = neko::ui::RunSteadyState(worker, options);
  if (!state.ready) {
    std::fprintf(stderr,
                 "warning: the page did not settle within %lld ms; the numbers below "
                 "describe a load in progress, not steady state\n",
                 static_cast<long long>(options.ready_timeout.count()));
  }

  const neko::browser::RuntimeStats runtime = worker.SnapshotRuntimeStats();
  const neko::network::HttpClient::Stats network = neko::network::SharedHttpClient().stats();
  neko::browser::PerfReport report;
  report.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - wall_start)
                       .count();
  report.pump = runtime.pump;
  report.frames = runtime.frames;
  report.counters.requests = network.requests;
  report.counters.connections_opened = network.connections_opened;
  report.counters.connections_reused = network.connections_reused;
  report.counters.connections_discarded = network.connections_discarded;
  report.counters.bytes = network.bytes;

  if (!quiet) {
    const neko::browser::LatencySummary production =
        neko::browser::Summarize(report.frames.production);
    const neko::browser::LatencySummary interval = neko::browser::Summarize(report.frames.interval);
    std::printf(
        "ready after %lld ms; steady-state %.1f s: frames %zu (production p50 %lld / max %lld ms), "
        "interval p50 %lld / max %lld ms, pump turns %zu (longest %lld ms, over 50 ms %zu)\n",
        state.ready_ms,
        static_cast<double>(report.wall_ms) / 1000.0,
        report.frames.frames,
        production.p50,
        production.max,
        interval.p50,
        interval.max,
        report.pump.turns.count,
        report.pump.turns.longest_ms,
        report.pump.turns.over_50ms);
  }
  std::printf("%s\n", neko::browser::FormatPerfReportLine(report).c_str());
  return 0;
}
