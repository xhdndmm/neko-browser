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
  report.phases.stylesheets2 = 617;
  report.phases.fonts2 = 0;
  report.phases.images = 559;
  report.phases.videos = 0;
  report.phases.post_load = 4162;
  return report;
}

TEST(PerfReportTest, FormatsFixedKeyOrderOnOneLine)
{
  const std::string line = FormatPerfReportLine(SampleReport());
  EXPECT_EQ(line,
            "{\"wall_ms\":1234,\"requests\":181,\"connections_opened\":49,"
            "\"connections_reused\":132,\"connections_discarded\":27,\"bytes\":19233480,"
            "\"phases\":{\"document\":202,\"stylesheets1\":975,\"fonts1\":899,"
            "\"scripts\":6442,\"stylesheets2\":617,\"fonts2\":0,\"images\":559,"
            "\"videos\":0,\"post_load\":4162}}");
  EXPECT_EQ(line.find('\n'), std::string::npos);
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
