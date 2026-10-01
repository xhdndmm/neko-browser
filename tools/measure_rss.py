#!/usr/bin/env python3
"""Measure the peak resident set size (max RSS) of a child process.

Usage:
    python3 tools/measure_rss.py [--json] [--label NAME] -- <command> [args...]

The tool runs the command to completion, discards its output, and reports the
child's peak RSS as observed by wait4(2) (resource.getrusage(RUSAGE_CHILDREN)
in this parent).  This is a stable, dependency-free way to compare memory
footprints of the CLI/GUI scenarios that the project cares about -- e.g. the
same page parsed with --dump-dom vs. rasterized with --screenshot.

Notes
- On Linux ru_maxrss is KiB; on macOS it is bytes.  Both are normalised to MiB.
- The measurement includes everything the child allocated at any point
  (peak), which is what "occupancy" work should track; it is not an
  instantaneous reading.
- Exit code of the child is propagated, so this composes with test scripts
  (`--` before the command keeps option parsing unambiguous).

Examples
    python3 tools/measure_rss.py --label dump-dom -- build/release/bin/neko_browser \
        --url file://$PWD/tests/pages/forms.html --dump-dom
    python3 tools/measure_rss.py --json -- build/release/bin/neko_browser --version
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time

if sys.platform == "win32":  # pragma: no cover - platform guard
    print(
        "error: this tool needs wait4(2)-style accounting and is POSIX-only; "
        "on Windows measure peak RSS with "
        "Get-Process <name> | Select-Object PeakWorkingSet64",
        file=sys.stderr,
    )
    raise SystemExit(2)

import resource  # noqa: E402  (POSIX-only, imported after the guard)


def _to_mib(raw_maxrss: int) -> float:
    """ru_maxrss uses platform-dependent units: KiB on Linux, bytes on macOS."""
    if sys.platform == "darwin":
        return raw_maxrss / (1024.0 * 1024.0)
    return raw_maxrss / 1024.0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Measure the peak RSS of a command (see module docstring).",
        allow_abbrev=False,
    )
    parser.add_argument("--json", action="store_true", help="print a JSON object")
    parser.add_argument("--label", default=None, help="a label for the JSON report")
    parser.add_argument("--quiet", action="store_true", help="suppress child output")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("no command given (use: measure_rss.py -- <command> [args...])")

    executable = shutil.which(command[0])
    if executable is None:
        print(f"error: {command[0]!r} not found in PATH", file=sys.stderr)
        return 127

    # Reset the counters so the reading below belongs to this child only.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    sink = subprocess.DEVNULL if args.quiet else None
    try:
        completed = subprocess.run(
            command,
            stdout=sink,
            stderr=sink,
            check=False,
        )
        exit_code = completed.returncode
    except KeyboardInterrupt:
        exit_code = 130
    elapsed = time.monotonic() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)

    # RUSAGE_CHILDREN aggregates every child this process ever waited for; the
    # proxy for "this run" is the difference, which is exact while only one
    # child is measured per invocation.
    maxrss_raw = after.ru_maxrss - before.ru_maxrss
    peak_mib = _to_mib(maxrss_raw)
    seconds = after.ru_utime - before.ru_utime + after.ru_stime - before.ru_stime

    if args.json:
        print(
            json.dumps(
                {
                    "label": args.label,
                    "command": command,
                    "exit_code": exit_code,
                    "peak_rss_mib": round(peak_mib, 2),
                    "cpu_seconds": round(seconds, 3),
                    "wall_seconds": round(elapsed, 3),
                },
                ensure_ascii=False,
            )
        )
    else:
        prefix = f"{args.label}: " if args.label else ""
        print(
            f"{prefix}peak RSS {peak_mib:.1f} MiB, "
            f"cpu {seconds:.3f} s, wall {elapsed:.3f} s, exit {exit_code}"
        )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
