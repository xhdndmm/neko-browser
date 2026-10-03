// neko::browser measurement primitives — see perf_report.h for the contract.

#include "neko/browser/perf_report.h"

#include <algorithm>

namespace neko::browser {
namespace {

void AppendSummary(std::string& out, const char* prefix, const LatencyTracker& tracker)
{
  const LatencySummary summary = Summarize(tracker);
  out += '"';
  out += prefix;
  out += "_p50\":";
  out += std::to_string(summary.p50);
  out += ",\"";
  out += prefix;
  out += "_p95\":";
  out += std::to_string(summary.p95);
  out += ",\"";
  out += prefix;
  out += "_max\":";
  out += std::to_string(summary.max);
  out += ',';
}

} // namespace

void LatencyTracker::Record(long long ms)
{
  ++count;
  total_ms += ms;
  longest_ms = std::max(longest_ms, ms);
  if (ms >= kLongActionMs) {
    ++over_50ms;
  }
  if (samples_ms.size() < kMaxSamples) {
    samples_ms.push_back(ms);
  }
}

LatencySummary Summarize(const LatencyTracker& tracker)
{
  LatencySummary summary;
  if (tracker.samples_ms.empty()) {
    return summary;
  }
  std::vector<long long> sorted = tracker.samples_ms;
  std::sort(sorted.begin(), sorted.end());
  const std::size_t last = sorted.size() - 1;
  // Nearest-rank: rank = ceil(p * n) (1-based), clamped into the series.
  const auto nearest_rank = [&sorted](double percentile) {
    const double rank = percentile * static_cast<double>(sorted.size());
    const std::size_t index = static_cast<std::size_t>(rank) == rank
                                  ? static_cast<std::size_t>(rank) - 1
                                  : static_cast<std::size_t>(rank);
    return sorted[std::min(index, sorted.size() - 1)];
  };
  summary.p50 = nearest_rank(0.50);
  summary.p95 = nearest_rank(0.95);
  summary.max = sorted[last];
  return summary;
}

void MergeLatency(LatencyTracker& into, const LatencyTracker& from)
{
  into.count += from.count;
  into.over_50ms += from.over_50ms;
  into.total_ms += from.total_ms;
  into.longest_ms = std::max(into.longest_ms, from.longest_ms);
  for (const long long sample : from.samples_ms) {
    if (into.samples_ms.size() >= LatencyTracker::kMaxSamples) {
      break;
    }
    into.samples_ms.push_back(sample);
  }
}

std::string FormatPerfReportLine(const PerfReport& report)
{
  std::string out = "{\"wall_ms\":";
  out += std::to_string(report.wall_ms);
  out += ",\"requests\":";
  out += std::to_string(report.counters.requests);
  out += ",\"connections_opened\":";
  out += std::to_string(report.counters.connections_opened);
  out += ",\"connections_reused\":";
  out += std::to_string(report.counters.connections_reused);
  out += ",\"connections_discarded\":";
  out += std::to_string(report.counters.connections_discarded);
  out += ",\"bytes\":";
  out += std::to_string(report.counters.bytes);

  out += ",\"phases\":{\"document\":";
  out += std::to_string(report.phases.document);
  out += ",\"stylesheets1\":";
  out += std::to_string(report.phases.stylesheets1);
  out += ",\"fonts1\":";
  out += std::to_string(report.phases.fonts1);
  out += ",\"scripts\":";
  out += std::to_string(report.phases.scripts);
  out += ",\"scripts_fetch\":";
  out += std::to_string(report.phases.scripts_fetch);
  out += ",\"scripts_exec\":";
  out += std::to_string(report.phases.scripts_exec);
  out += ",\"scripts_modules\":";
  out += std::to_string(report.phases.scripts_modules);
  out += ",\"scripts_events\":";
  out += std::to_string(report.phases.scripts_events);
  out += ",\"scripts_restyle\":";
  out += std::to_string(report.phases.scripts_restyle);
  out += ",\"stylesheets2\":";
  out += std::to_string(report.phases.stylesheets2);
  out += ",\"fonts2\":";
  out += std::to_string(report.phases.fonts2);
  out += ",\"images\":";
  out += std::to_string(report.phases.images);
  out += ",\"videos\":";
  out += std::to_string(report.phases.videos);
  out += ",\"pump\":";
  out += std::to_string(report.phases.pump);
  out += ",\"late_images\":";
  out += std::to_string(report.phases.late_images);
  out += ",\"post_load\":";
  out += std::to_string(report.phases.post_load);
  out += '}';

  out += ",\"pump_turns\":";
  out += std::to_string(report.pump.count);
  out += ",\"pump_over_50ms\":";
  out += std::to_string(report.pump.over_50ms);
  out += ",\"pump_total_ms\":";
  out += std::to_string(report.pump.total_ms);
  out += ',';
  AppendSummary(out, "pump", report.pump);

  out += "\"frames\":";
  out += std::to_string(report.frames.frames);
  out += ",\"frame_over_50ms\":";
  out += std::to_string(report.frames.production.over_50ms);
  out += ',';
  AppendSummary(out, "frame_production", report.frames.production);
  AppendSummary(out, "frame_interval", report.frames.interval);
  // AppendSummary leaves a trailing comma; close the object instead.
  out.back() = '}';
  return out;
}

} // namespace neko::browser
