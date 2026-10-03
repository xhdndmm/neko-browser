// neko::browser::FormatPerfReportLine — the --perf-report artifact format.
//
// The line is consumed by scripts and pasted into docs/development/
// perf-baselines.md, so its exact shape is part of the contract: a fixed key
// order (textually diffable) and no embedded newline.

#include "neko/browser/perf_report.h"

#include <gtest/gtest.h>
#include <string>

namespace neko::browser {
namespace {

PerfReport SampleReport()
{
  PerfReport report;
  report.wall_ms = 1234;
  report.counters.requests = 181;
  report.counters.connections_opened = 49;
  report.counters.connections_reused = 132;
  report.counters.connections_discarded = 27;
  report.counters.bytes = 19233480;
  report.phases.document = 202;
  report.phases.stylesheets1 = 975;
  report.phases.fonts1 = 899;
  report.phases.scripts = 6442;
  report.phases.scripts_fetch = 215;
  report.phases.scripts_exec = 130;
  report.phases.scripts_modules = 5057;
  report.phases.scripts_events = 0;
  report.phases.scripts_restyle = 500;
  report.phases.stylesheets2 = 617;
  report.phases.fonts2 = 0;
  report.phases.images = 559;
  report.phases.videos = 0;
  report.phases.pump = 5607;
  report.phases.late_images = 293;
  report.phases.post_load = 5900;
  report.pump.turns.Record(0);
  report.pump.turns.Record(4726);
  for (int i = 0; i < 14; ++i) {
    report.pump.turns.Record(0);
  }
  report.pump.breakdown.timers = 95;
  report.pump.breakdown.timers_ms = 372;
  report.pump.breakdown.job_rounds = 95;
  report.pump.breakdown.jobs_ms = 104;
  report.pump.breakdown.xhr = 8;
  report.pump.breakdown.xhr_ms = 0;
  report.pump.breakdown.raf = 38;
  report.pump.breakdown.raf_ms = 2494;
  report.frames.frames = 3;
  report.frames.production.Record(18);
  report.frames.interval.Record(817);
  report.frames.interval.Record(1815);
  return report;
}

TEST(PerfReportTest, FormatsFixedKeyOrderOnOneLine)
{
  const std::string line = FormatPerfReportLine(SampleReport());
  EXPECT_EQ(line,
            "{\"wall_ms\":1234,\"requests\":181,\"connections_opened\":49,"
            "\"connections_reused\":132,\"connections_discarded\":27,\"bytes\":19233480,"
            "\"phases\":{\"document\":202,\"stylesheets1\":975,\"fonts1\":899,"
            "\"scripts\":6442,\"scripts_fetch\":215,\"scripts_exec\":130,"
            "\"scripts_modules\":5057,\"scripts_events\":0,\"scripts_restyle\":500,"
            "\"stylesheets2\":617,\"fonts2\":0,\"images\":559,"
            "\"videos\":0,\"pump\":5607,\"late_images\":293,\"post_load\":5900},"
            "\"pump_turns\":16,\"pump_over_50ms\":1,\"pump_total_ms\":4726,"
            "\"pump_p50\":0,\"pump_p95\":4726,\"pump_max\":4726,"
            "\"pump_breakdown\":{\"timers_ms\":372,\"timers\":95,\"jobs_ms\":104,"
            "\"job_rounds\":95,\"xhr_ms\":0,\"xhr\":8,\"raf_ms\":2494,"
            "\"raf\":38,\"ports_ms\":0,\"ports\":0,\"websockets_ms\":0,"
            "\"websockets\":0},"
            "\"frames\":3,\"frame_over_50ms\":0,"
            "\"frame_production_p50\":18,\"frame_production_p95\":18,"
            "\"frame_production_max\":18,"
            "\"frame_interval_p50\":817,\"frame_interval_p95\":1815,"
            "\"frame_interval_max\":1815}");
  EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST(PerfReportTest, LatencyTrackerCountsAndCapsSamples)
{
  LatencyTracker tracker;
  tracker.Record(5);
  tracker.Record(60);
  tracker.Record(7);
  EXPECT_EQ(tracker.count, 3u);
  EXPECT_EQ(tracker.over_50ms, 1u);
  EXPECT_EQ(tracker.total_ms, 72);
  EXPECT_EQ(tracker.longest_ms, 60);
  EXPECT_EQ(tracker.samples_ms.size(), 3u);

  // The series is capped, but the counters keep counting: a report can always
  // say how many actions were seen and whether the percentiles cover them.
  for (std::size_t i = tracker.samples_ms.size(); i < LatencyTracker::kMaxSamples + 10; ++i) {
    tracker.Record(1);
  }
  EXPECT_EQ(tracker.samples_ms.size(), LatencyTracker::kMaxSamples);
  EXPECT_GT(tracker.count, LatencyTracker::kMaxSamples);
}

TEST(PerfReportTest, SummarizeUsesNearestRankPercentiles)
{
  LatencyTracker tracker;
  for (const long long ms : {10, 20, 30, 40}) {
    tracker.Record(ms);
  }
  const LatencySummary summary = Summarize(tracker);
  EXPECT_EQ(summary.p50, 20); // ceil(0.50 * 4) = rank 2
  EXPECT_EQ(summary.p95, 40); // ceil(0.95 * 4) = rank 4
  EXPECT_EQ(summary.max, 40);

  EXPECT_EQ(Summarize(LatencyTracker{}).max, 0); // no samples: zeros, not a crash
}

TEST(PerfReportTest, MergeLatencyAddsCountersAndAppendsSamples)
{
  LatencyTracker into;
  into.Record(10);
  LatencyTracker from;
  from.Record(700);
  from.Record(2);
  MergeLatency(into, from);
  EXPECT_EQ(into.count, 3u);
  EXPECT_EQ(into.total_ms, 712);
  EXPECT_EQ(into.longest_ms, 700);
  EXPECT_EQ(into.samples_ms.size(), 3u);

  // Merging must not push the series past the cap.
  LatencyTracker full;
  for (std::size_t i = 0; i < LatencyTracker::kMaxSamples + 5; ++i) {
    full.Record(3);
  }
  MergeLatency(into, full);
  EXPECT_EQ(into.samples_ms.size(), LatencyTracker::kMaxSamples);
}

TEST(PerfReportTest, DefaultsAreZeroAndTheEnvelopeIsBalanced)
{
  const std::string line = FormatPerfReportLine(PerfReport{});
  EXPECT_NE(line.find("\"wall_ms\":0"), std::string::npos);
  EXPECT_NE(line.find("\"requests\":0"), std::string::npos);
  EXPECT_NE(line.find("\"connections_reused\":0"), std::string::npos);
  EXPECT_NE(line.find("\"post_load\":0"), std::string::npos);
  EXPECT_EQ(line.front(), '{');
  EXPECT_EQ(line.back(), '}');
  // Cheap structural guard without a JSON dependency: a stray brace would be a
  // silent format break for the scripts that read this line.
  int depth = 0;
  for (const char c : line) {
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
    }
    EXPECT_GE(depth, 0);
  }
  EXPECT_EQ(depth, 0);
}

} // namespace
} // namespace neko::browser
