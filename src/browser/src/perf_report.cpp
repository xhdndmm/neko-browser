// neko::browser::FormatPerfReportLine — the --perf-report artifact format.
//
// Hand-rolled JSON: the payload is a flat object of integers, so a JSON library
// would add a dependency for nothing, and a fixed key order keeps two reports
// textually diffable.

#include "neko/browser/perf_report.h"

namespace neko::browser {
namespace {

void AppendPhase(std::string& out, const char* name, long long value, bool last)
{
  out += '"';
  out += name;
  out += "\":";
  out += std::to_string(value);
  if (!last) {
    out += ',';
  }
}

} // namespace

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
  out += ",\"phases\":{";
  AppendPhase(out, "document", report.phases.document, false);
  AppendPhase(out, "stylesheets1", report.phases.stylesheets1, false);
  AppendPhase(out, "fonts1", report.phases.fonts1, false);
  AppendPhase(out, "scripts", report.phases.scripts, false);
  AppendPhase(out, "stylesheets2", report.phases.stylesheets2, false);
  AppendPhase(out, "fonts2", report.phases.fonts2, false);
  AppendPhase(out, "images", report.phases.images, false);
  AppendPhase(out, "videos", report.phases.videos, false);
  AppendPhase(out, "post_load", report.phases.post_load, true);
  out += "}}";
  return out;
}

} // namespace neko::browser
