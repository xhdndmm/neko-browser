#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace neko::browser {

// Deterministic page-load phase timings (milliseconds).  Zero means the phase
// did not run (for example a page with no fonts, or a local-file load).
struct LoadPhases
{
  long long document = 0;             // top-level document fetch
  long long stylesheets1 = 0;         // external <link rel=stylesheet> pass
  long long fonts1 = 0;               // @font-face pass
  long long scripts = 0;              // whole script phase (RunPageScripts)
  long long scripts_fetch = 0;        // ...of which: blocked waiting for script bodies
  long long scripts_exec = 0;         // ...of which: classic script parse/compile/run
  long long scripts_modules = 0;      // ...of which: module scripts (fetch + evaluate)
  long long scripts_modules_wait = 0; // ...of which: blocked in apis.fetch
  long long scripts_modules_eval = 0; // ...of which: compile + run
  std::size_t sync_fetch_calls = 0;   // apis.fetch calls in the script phase
  long long scripts_events = 0;       // ...of which: DOMContentLoaded/load handlers
  long long scripts_restyle = 0;      // ...of which: cascade re-run after scripts
  long long stylesheets2 = 0;         // script-injected stylesheets
  long long fonts2 = 0;               // fonts declared by script-injected sheets
  long long images = 0;               // <img>/<video> subresources
  long long videos = 0;               // video subresources
  long long pump = 0;                 // script/event pump (PumpScriptTimersUntilQuiet)
  long long late_images = 0;          // late image rounds after the pump
  long long post_load = 0;            // pump + late_images (kept as the total)
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

// A capped series of action durations plus counters.
//
// "Action" is the unit a user perceives as a long task: one pump turn
// (DomBinder::RunPendingTimers, which runs the due timers and delivers queued
// XHR completions) or one frame production.  The series is capped so a browser
// left running for hours cannot grow it without bound; |count| keeps counting
// past the cap, so a report can always say how many actions were observed and
// whether the percentiles cover all of them.
struct LatencyTracker
{
  static constexpr std::size_t kMaxSamples = 4096;
  // Informational only: nothing gates on this (TESTING.md, "时间相关测试").
  static constexpr long long kLongActionMs = 50;

  std::size_t count = 0;     // every observation
  std::size_t over_50ms = 0; // observations at or above kLongActionMs
  long long total_ms = 0;
  long long longest_ms = 0;
  std::vector<long long> samples_ms; // first kMaxSamples observations

  void Record(long long ms);
};

// Exact percentiles (nearest-rank) over the recorded samples.
struct LatencySummary
{
  long long p50 = 0;
  long long p95 = 0;
  long long max = 0;
};

LatencySummary Summarize(const LatencyTracker& tracker);

// Frames produced on the worker thread: how long production took, and how far
// apart consecutive frames were.  Only a frame pipeline fills these (the GUI
// path); a headless DOM dump produces no frames and reports zeros.
struct FrameStats
{
  std::size_t frames = 0;
  LatencyTracker production; // ProduceFrame() cost
  LatencyTracker interval;   // steady_clock gap between frames
};

// Adds |from| into |into| (counters always; samples up to the cap).  Used to
// publish a locally measured batch without holding a lock while it is measured:
// the pump and frame paths measure into a local tracker and merge once, so a
// concurrent reader never sees a half-written series.
void MergeLatency(LatencyTracker& into, const LatencyTracker& from);

// What the pump turns actually ran, mirrored from the JS layer's measurement
// (javascript::DomBinder::TurnBreakdown).  A four-second turn tells you nothing
// until you know which segment owns it -- on a real page the answer decides
// whether the fix belongs in XHR delivery, in timer callbacks, or in promise
// continuations.
struct PumpBreakdown
{
  long long timers_ms = 0;
  std::size_t timers = 0;
  long long jobs_ms = 0;
  std::size_t job_rounds = 0;
  long long xhr_ms = 0;
  std::size_t xhr = 0;
  long long raf_ms = 0;
  std::size_t raf = 0;
  long long ports_ms = 0;
  std::size_t ports = 0;
  long long websockets_ms = 0;
  std::size_t websockets = 0;
};

// Pump measurement: per-turn durations (the smoothness signal) plus what those
// turns ran.  The CLI measures one load; the GUI accumulates across a session,
// which is why the pieces are merged rather than assigned.
struct PumpStats
{
  LatencyTracker turns;
  PumpBreakdown breakdown;
};

// What the worker side (BrowserController) publishes for the GUI path: the
// script/event pump turns it served and the frames it produced.  The headless
// CLI fills pump from its own load; only a frame pipeline fills |frames|.
struct RuntimeStats
{
  PumpStats pump;
  FrameStats frames;
};

void MergePump(PumpStats& into, const PumpStats& from);

struct PerfReport
{
  LoadPhases phases;
  LoadCounters counters;
  PumpStats pump; // pump turns observed by this run, and what they ran
  FrameStats frames;
  long long wall_ms = 0; // whole run, including a requested screenshot
};

// One JSON object on a single line, keys in a fixed order, so two reports can
// be diffed textually.
//
// This is the committed, reproducible form of a measurement: run the headless
// CLI with --perf-report (load pipeline) or neko_gui_screenshot with
// --perf-report (GUI frame pipeline) and compare against
// docs/development/perf-baselines.md.  It exists because a number produced by
// throwaway instrumentation cannot be re-checked by a later reader.
//
// Nothing gates CI on these values: they are evidence, not a threshold.  Timing
// gates on shared runners are forbidden (TESTING.md, "时间相关测试"；
// AGENTS.md 44) and counts gate only where a test asserts behaviour
// (see the connection-reuse tests in tests/unit/network).
std::string FormatPerfReportLine(const PerfReport& report);

} // namespace neko::browser
