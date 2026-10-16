// neko::browser measurement primitives — see perf_report.h for the contract.

#include "neko/browser/perf_report.h"

#include <algorithm>
#include <cmath>

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
    const std::size_t index =
        static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(sorted.size()))) - 1;
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

void MergePump(PumpStats& into, const PumpStats& from)
{
  MergeLatency(into.turns, from.turns);
  into.breakdown.timers_ms += from.breakdown.timers_ms;
  into.breakdown.timers += from.breakdown.timers;
  into.breakdown.jobs_ms += from.breakdown.jobs_ms;
  into.breakdown.job_rounds += from.breakdown.job_rounds;
  into.breakdown.xhr_ms += from.breakdown.xhr_ms;
  into.breakdown.xhr += from.breakdown.xhr;
  into.breakdown.raf_ms += from.breakdown.raf_ms;
  into.breakdown.raf += from.breakdown.raf;
  into.breakdown.ports_ms += from.breakdown.ports_ms;
  into.breakdown.ports += from.breakdown.ports;
  into.breakdown.websockets_ms += from.breakdown.websockets_ms;
  into.breakdown.websockets += from.breakdown.websockets;
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
  out += ",\"scripts_modules_wait\":";
  out += std::to_string(report.phases.scripts_modules_wait);
  out += ",\"scripts_modules_eval\":";
  out += std::to_string(report.phases.scripts_modules_eval);
  out += ",\"sync_fetch_calls\":";
  out += std::to_string(report.phases.sync_fetch_calls);
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
  out += std::to_string(report.pump.turns.count);
  out += ",\"pump_over_50ms\":";
  out += std::to_string(report.pump.turns.over_50ms);
  out += ",\"pump_total_ms\":";
  out += std::to_string(report.pump.turns.total_ms);
  out += ',';
  AppendSummary(out, "pump", report.pump.turns);
  // What the turns ran: the attribution a long turn needs.
  out += "\"pump_breakdown\":{\"timers_ms\":";
  out += std::to_string(report.pump.breakdown.timers_ms);
  out += ",\"timers\":";
  out += std::to_string(report.pump.breakdown.timers);
  out += ",\"jobs_ms\":";
  out += std::to_string(report.pump.breakdown.jobs_ms);
  out += ",\"job_rounds\":";
  out += std::to_string(report.pump.breakdown.job_rounds);
  out += ",\"xhr_ms\":";
  out += std::to_string(report.pump.breakdown.xhr_ms);
  out += ",\"xhr\":";
  out += std::to_string(report.pump.breakdown.xhr);
  out += ",\"raf_ms\":";
  out += std::to_string(report.pump.breakdown.raf_ms);
  out += ",\"raf\":";
  out += std::to_string(report.pump.breakdown.raf);
  out += ",\"ports_ms\":";
  out += std::to_string(report.pump.breakdown.ports_ms);
  out += ",\"ports\":";
  out += std::to_string(report.pump.breakdown.ports);
  out += ",\"websockets_ms\":";
  out += std::to_string(report.pump.breakdown.websockets_ms);
  out += ",\"websockets\":";
  out += std::to_string(report.pump.breakdown.websockets);
  out += "},";

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
