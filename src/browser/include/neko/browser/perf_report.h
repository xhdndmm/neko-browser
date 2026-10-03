#pragma once

#include <cstddef>
#include <string>

namespace neko::browser {

// Deterministic page-load phase timings (milliseconds).  Zero means the phase
// did not run (for example a page with no fonts, or a local-file load).
struct LoadPhases
{
  long long document = 0;     // top-level document fetch
  long long stylesheets1 = 0; // external <link rel=stylesheet> pass
  long long fonts1 = 0;       // @font-face pass
  long long scripts = 0;      // script fetch/execute (RunPageScripts)
  long long stylesheets2 = 0; // script-injected stylesheets
  long long fonts2 = 0;       // fonts declared by script-injected sheets
  long long images = 0;       // <img>/<video> subresources
  long long videos = 0;       // video subresources
  long long post_load = 0;    // timer pump + late image pass (no screenshot)
};

// Connection and payload accounting, mirroring network::HttpClient::Stats.
struct LoadCounters
{
  std::size_t requests = 0;
  std::size_t connections_opened = 0;
  std::size_t connections_reused = 0;
  std::size_t connections_discarded = 0;
  std::size_t bytes = 0; // decoded response bytes, all resources
};

struct PerfReport
{
  LoadPhases phases;
  LoadCounters counters;
  long long wall_ms = 0; // whole run, including a requested screenshot
};

// One JSON object on a single line, keys in a fixed order, so two reports can
// be diffed textually.
//
// This is the committed, reproducible form of a measurement: run the headless
// CLI with --perf-report and compare against
// docs/development/perf-baselines.md.  It exists because a number produced by
// throwaway instrumentation cannot be re-checked by a later reader.
//
// Nothing gates CI on these values: they are evidence, not a threshold.  Timing
// gates on shared runners are forbidden (TESTING.md, "时间相关测试"；
// AGENTS.md 44) and counts gate only where a test asserts behaviour
// (see the connection-reuse tests in tests/unit/network).
std::string FormatPerfReportLine(const PerfReport& report);

} // namespace neko::browser
