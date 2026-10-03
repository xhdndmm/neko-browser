#!/usr/bin/env python3
"""Summarize neko-browser --perf-report output.

The browser prints one JSON object per run (see docs/development/perf-baselines.md).
This reader accepts that output straight from the pipe — the surrounding DOM dump
is ignored — or from saved files, and prints:

  * the deterministic counters, which are what a before/after comparison should
    judge (requests, connections, bytes).  If two samples disagree on a counter
    the tool says so: either the page content rotated or something changed.
  * phase medians, plus the script-phase split and the pump breakdown, which are
    where "slow" turns into "slow at what".
  * frame numbers for the GUI path (neko_gui_screenshot --perf-report).

Times are evidence, never a gate: wall clock and per-turn milliseconds vary with
the machine and the network, and TESTING.md ("时间相关测试") plus AGENTS.md 44
forbid gating CI on them.  Counts are the gateable form.

Usage:
  neko_browser --headless --url URL --dump-dom --perf-report | tools/perf_report.py
  neko_gui_screenshot URL out.png --perf-report | tools/perf_report.py
  tools/perf_report.py /tmp/perf.jsonl /tmp/perf2.jsonl   # collected samples
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys

COUNTER_KEYS = (
    "requests",
    "connections_opened",
    "connections_reused",
    "connections_discarded",
    "bytes",
)

PHASE_KEYS = (
    "document",
    "stylesheets1",
    "fonts1",
    "scripts",
    "scripts_fetch",
    "scripts_exec",
    "scripts_modules",
    "scripts_events",
    "scripts_restyle",
    "stylesheets2",
    "fonts2",
    "images",
    "videos",
    "pump",
    "late_images",
    "post_load",
)

PUMP_BREAKDOWN_KEYS = (
    ("raf", "raf_ms"),
    ("timers", "timers_ms"),
    ("job_rounds", "jobs_ms"),
    ("xhr", "xhr_ms"),
    ("ports", "ports_ms"),
    ("websockets", "websockets_ms"),
)

FRAME_KEYS = (
    "frames",
    "frame_over_50ms",
    "frame_production_p50",
    "frame_production_max",
    "frame_interval_p50",
    "frame_interval_max",
)


def extract_reports(text: str) -> list[dict]:
    """Every report object found in |text|, in order.

    The browser mixes the line into its normal stdout (a DOM dump, "saved ..."),
    so each line is searched for the object's first key instead of being parsed
    whole.  Unparsable candidates are skipped rather than fatal: this is a
    measurement reader, not a validator.
    """
    reports: list[dict] = []
    for line in text.splitlines():
        start = line.find('{"wall_ms"')
        if start < 0:
            continue
        try:
            reports.append(json.loads(line[start:]))
        except json.JSONDecodeError:
            continue
    return reports


def median_of(reports: list[dict], key: str) -> float:
    return statistics.median(report.get(key, 0) for report in reports)


def median_phase(reports: list[dict], key: str) -> float:
    return statistics.median(
        report.get("phases", {}).get(key, 0) for report in reports
    )


def print_counters(reports: list[dict]) -> None:
    print(f"counters ({len(reports)} sample(s); deterministic — compare these):")
    for key in COUNTER_KEYS:
        values = [report.get(key, 0) for report in reports]
        spread = "" if len(set(values)) == 1 else f"   <- differs across samples: {values}"
        shown = values[0]
        if key == "bytes":
            print(f"  {key:22s} {shown:>12d}  ({shown / 1048576:.1f} MB{spread})")
        else:
            print(f"  {key:22s} {shown:>12d}{spread}")


def print_phases(reports: list[dict], unit: str) -> None:
    print(f"\nphases (median {unit}; evidence, not a gate):")
    for key in PHASE_KEYS:
        value = median_phase(reports, key)
        if value or key in ("scripts", "post_load", "pump"):
            print(f"  {key:22s} {value:>9.0f} {unit}")


def print_pump(reports: list[dict], unit: str) -> None:
    print(f"\npump turns (median; the smoothness signal):")
    print(f"  {'turns':22s} {median_of(reports, 'pump_turns'):>9.0f}")
    print(f"  {'over 50 ms':22s} {median_of(reports, 'pump_over_50ms'):>9.0f}")
    print(f"  {'total':22s} {median_of(reports, 'pump_total_ms'):>9.0f} {unit}")
    print(f"  {'longest turn':22s} {median_of(reports, 'pump_max'):>9.0f} {unit}")

    breakdowns = [report.get("pump_breakdown") for report in reports]
    breakdowns = [item for item in breakdowns if isinstance(item, dict)]
    if not breakdowns:
        print("  (this build predates the pump breakdown; rebuild to get it)")
        return
    print("  what the turns ran (median count / median ms):")
    for count_key, ms_key in PUMP_BREAKDOWN_KEYS:
        count = statistics.median(item.get(count_key, 0) for item in breakdowns)
        millis = statistics.median(item.get(ms_key, 0) for item in breakdowns)
        if count or millis:
            print(f"    {count_key:20s} {count:>7.0f} / {millis:>7.0f} {unit}")


def print_frames(reports: list[dict], unit: str) -> None:
    values = {key: median_of(reports, key) for key in FRAME_KEYS}
    if not any(values.values()):
        print(
            "\nframes: none — expected for the headless CLI "
            "(use neko_gui_screenshot --perf-report for the GUI path)"
        )
        return
    print(f"\nframes (median {unit}; GUI path):")
    for key in FRAME_KEYS:
        print(f"  {key:22s} {values[key]:>9.0f}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize --perf-report lines (see the module docstring)."
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="files holding report lines; stdin when omitted",
    )
    parser.add_argument("--raw", action="store_true", help="also echo the raw objects")
    parser.add_argument(
        "--unit", default=None, choices=["ms", "s"], help="time unit for the output"
    )
    args = parser.parse_args()

    if args.paths:
        text = "\n".join(open(path, encoding="utf-8", errors="replace").read() for path in args.paths)
    else:
        text = sys.stdin.read()

    reports = extract_reports(text)
    if not reports:
        print(
            "error: no --perf-report object found (is --perf-report missing, or an "
            "old build without the flag?)",
            file=sys.stderr,
        )
        return 2

    unit = args.unit
    if unit is None:
        unit = "ms"
    divisor = 1000.0 if unit == "s" else 1.0

    def scale(report: dict) -> dict:
        # Scale every time-valued field once, so the printers stay simple.
        if divisor == 1.0:
            return report
        scaled = json.loads(json.dumps(report))
        for key in ("wall_ms", "pump_total_ms", "pump_max", "pump_p50", "pump_p95"):
            if key in scaled:
                scaled[key] = scaled[key] / divisor
        for key, value in list(scaled.get("phases", {}).items()):
            scaled["phases"][key] = value / divisor
        for key, value in list(scaled.get("pump_breakdown", {}).items()):
            if key.endswith("_ms"):
                scaled["pump_breakdown"][key] = value / divisor
        for key in ("frame_production_p50", "frame_production_p95", "frame_production_max",
                    "frame_interval_p50", "frame_interval_p95", "frame_interval_max"):
            if key in scaled:
                scaled[key] = scaled[key] / divisor
        return scaled

    reports = [scale(report) for report in reports]

    print(f"wall: {median_of(reports, 'wall_ms'):.1f} {unit} median ({len(reports)} sample(s))")
    print_counters(reports)
    print_phases(reports, unit)
    print_pump(reports, unit)
    print_frames(reports, unit)

    if args.raw:
        print("\nraw:")
        for report in reports:
            print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
